#include "talk/base/helpers.h"
#include "ppsession.h"

namespace {
    const uint32 MSG_TIMEOUT = 1; 
    const uint32 MSG_ERROR = 2; 
    const uint32 MSG_STATE = 3;
}  // namespace

using namespace cricket;

PPSession::PPSession( const std::string& sid,
                      const std::string& content_type,
                      bool isInitialtor)
    : BaseSession(NULL, //session_manager->signaling_thread(),
                  NULL, //session_manager->worker_thread(),
                  NULL, //session_manager->port_allocator(),
                  sid, content_type, isInitialtor) {
  initiate_acked_ = false;
}

PPSession::~PPSession() {

}

void PPSession::OnMessage(talk_base::Message *pmsg) {
  // preserve this because BaseSession::OnMessage may modify it
  State orig_state = state();

  BaseSession::OnMessage(pmsg);

  switch (pmsg->message_id) {
  case MSG_ERROR:
    TerminateWithReason(STR_TERMINATE_ERROR);
    break;

  case MSG_STATE:
    switch (orig_state) {
    case STATE_SENTREJECT:
    case STATE_RECEIVEDREJECT:
      // Assume clean termination.
      Terminate();
      break;

    case STATE_SENTTERMINATE:
    case STATE_RECEIVEDTERMINATE:
      //session_manager_->DestroySession(this); FIXME      
      break;

    default:
      // Explicitly ignoring some states here.
      break;
    }
    break;
  }
}

bool PPSession::Initiate(const SessionDescription* sdesc) {
  ASSERT(signaling_thread()->IsCurrent());
  SessionError error;

  // Only from STATE_INIT
  if (state() != STATE_INIT)
    return false;

  // Setup for signaling.
  set_local_description(sdesc);
  if (!CreateTransportProxies(GetEmptyTransportInfos(sdesc->contents()),
                              &error)) {
    LOG(LS_ERROR) << "Could not create transports: " << error.text;
    return false;
  }

  if (!SendInitiateMessage(sdesc, &error)) {
    LOG(LS_ERROR) << "Could not send initiate message: " << error.text;
    return false;
  }

  SetState(Session::STATE_SENTINITIATE);

  SpeculativelyConnectAllTransportChannels();
  return true;
}

bool PPSession::Accept(const SessionDescription* sdesc) {
  ASSERT(signaling_thread()->IsCurrent());

  // Only if just received initiate
  if (state() != STATE_RECEIVEDINITIATE)
    return false;

  // Setup for signaling.
  set_local_description(sdesc);

  SessionError error;
  if (!SendAcceptMessage(sdesc, &error)) {
    LOG(LS_ERROR) << "Could not send accept message: " << error.text;
    return false;
  }
  // TODO - Add BUNDLE support to transport-info messages.
  MaybeEnableMuxingSupport();  // Enable transport channel mux if supported.
  SetState(Session::STATE_SENTACCEPT);
  return true;
}

bool PPSession::Reject(const std::string& reason) {
  ASSERT(signaling_thread()->IsCurrent());

  // Reject is sent in response to an initiate or modify, to reject the
  // request
  if (state() != STATE_RECEIVEDINITIATE && state() != STATE_RECEIVEDMODIFY)
    return false;

  SessionError error;
  if (!SendRejectMessage(reason, &error)) {
    LOG(LS_ERROR) << "Could not send reject message: " << error.text;
    return false;
  }

  SetState(STATE_SENTREJECT);
  return true;
}

bool PPSession::TerminateWithReason(const std::string& reason) {
  ASSERT(signaling_thread()->IsCurrent());

  // Either side can terminate, at any time.
  switch (state()) {
    case STATE_SENTTERMINATE:
    case STATE_RECEIVEDTERMINATE:
      return false;

    case STATE_SENTREJECT:
    case STATE_RECEIVEDREJECT:
      // We don't need to send terminate if we sent or received a reject...
      // it's implicit.
      break;

    default:
      SessionError error;
      if (!SendTerminateMessage(reason, &error)) {
        LOG(LS_ERROR) << "Could not send terminate message: " << error.text;
        return false;
      }
      break;
  }

  SetState(STATE_SENTTERMINATE);
  return true;
}

bool PPSession::SendInfoMessage(const XmlElements& elems) {
  ASSERT(signaling_thread()->IsCurrent());
  SessionError error;
  if (!SendMessage(ACTION_SESSION_INFO, elems, &error)) {
    LOG(LS_ERROR) << "Could not send info message " << error.text;
    return false;
  }
  return true;
}

TransportInfos PPSession::GetEmptyTransportInfos(
    const ContentInfos& contents) const {
  TransportInfos tinfos;
  for (ContentInfos::const_iterator content = contents.begin();
       content != contents.end(); ++content) {
    tinfos.push_back(
        TransportInfo(content->name, transport_type(), Candidates()));
  }
  return tinfos;
}

bool PPSession::OnRemoteCandidates(
    const TransportInfos& tinfos, ParseError* error) {
  for (TransportInfos::const_iterator tinfo = tinfos.begin();
       tinfo != tinfos.end(); ++tinfo) {
    TransportProxy* transproxy = GetTransportProxy(tinfo->content_name);
    if (transproxy == NULL) {
      return BadParse("Unknown content name: " + tinfo->content_name, error);
    }

    // Must complete negotiation before sending remote candidates, or
    // there won't be any channel impls.
    transproxy->CompleteNegotiation();
    for (Candidates::const_iterator cand = tinfo->candidates.begin();
         cand != tinfo->candidates.end(); ++cand) {
      if (!transproxy->impl()->VerifyCandidate(*cand, error))
        return false;

      if (!transproxy->impl()->HasChannel(cand->name())) {
        buzz::XmlElement* extra_info =
            new buzz::XmlElement(QN_GINGLE_P2P_UNKNOWN_CHANNEL_NAME);
        extra_info->AddAttr(buzz::QN_NAME, cand->name());
        error->extra = extra_info;

        return BadParse("channel named in candidate does not exist: " +
                        cand->name() + " for content: "+ tinfo->content_name,
                        error);
      }
    }
    transproxy->impl()->OnRemoteCandidates(tinfo->candidates);
  }

  return true;
}

bool PPSession::CreateTransportProxies(const TransportInfos& tinfos,
                                     SessionError* error) {
  for (TransportInfos::const_iterator tinfo = tinfos.begin();
       tinfo != tinfos.end(); ++tinfo) {
    if (tinfo->transport_type != transport_type()) {
      error->SetText("No supported transport in offer.");
      return false;
    }

    GetOrCreateTransportProxy(tinfo->content_name);
  }
  return true;
}

void PPSession::OnTransportRequestSignaling(Transport* transport) {
  ASSERT(signaling_thread()->IsCurrent());
  SignalRequestSignaling(this);
}

void PPSession::OnTransportConnecting(Transport* transport) {
  // This is an indication that we should begin watching the writability
  // state of the transport.
  OnTransportWritable(transport);
}

void PPSession::OnTransportWritable(Transport* transport) {
  ASSERT(signaling_thread()->IsCurrent());

  // If the transport is not writable, start a timer to make sure that it
  // becomes writable within a reasonable amount of time.  If it does not, we
  // terminate since we can't actually send data.  If the transport is writable,
  // cancel the timer.  Note that writability transitions may occur repeatedly
  // during the lifetime of the session.
  signaling_thread()->Clear(this, MSG_TIMEOUT);
  if (transport->HasChannels() && !transport->writable()) {
    signaling_thread()->PostDelayed(
        10 * 1000, this, MSG_TIMEOUT);
  }
}

void PPSession::OnTransportCandidatesReady(Transport* transport,
                                         const Candidates& candidates) {
  ASSERT(signaling_thread()->IsCurrent());
  TransportProxy* transproxy = GetTransportProxy(transport);
  if (transproxy != NULL) {
    if (initiator() && !initiate_acked_) {
      // TODO: This is to work around server re-ordering
      // messages.  We send the candidates once the session-initiate
      // is acked.  Once we have fixed the server to guarantee message
      // order, we can remove this case.
      transproxy->AddUnsentCandidates(candidates);
    } else {
      if (!transproxy->negotiated()) {
        transproxy->AddSentCandidates(candidates);
      }
      SessionError error;
      if (!SendTransportInfoMessage(transproxy, candidates, &error)) {
        LOG(LS_ERROR) << "Could not send transport info message: "
                      << error.text;
        return;
      }
    }
  }
}

void PPSession::OnTransportSendError(Transport* transport,
                                   const buzz::XmlElement* stanza,
                                   const buzz::QName& name,
                                   const std::string& type,
                                   const std::string& text,
                                   const buzz::XmlElement* extra_info) {
  ASSERT(signaling_thread()->IsCurrent());
  SignalErrorMessage(this, stanza, name, type, text, extra_info);
}

void PPSession::OnTransportChannelGone(Transport* transport,
                                     const std::string& name) {
  ASSERT(signaling_thread()->IsCurrent());
  SignalChannelGone(this, name);
}

void PPSession::OnIncomingMessage(const SessionMessage& msg) {
  ASSERT(signaling_thread()->IsCurrent());
  //ASSERT(state() == STATE_INIT || msg.from == remote_name());

  bool valid = false;
  MessageError error;
  switch (msg.type) {
    case ACTION_SESSION_INITIATE:
      valid = OnInitiateMessage(msg, &error);
      break;
    case ACTION_SESSION_INFO:
      valid = OnInfoMessage(msg);
      break;
    case ACTION_SESSION_ACCEPT:
      valid = OnAcceptMessage(msg, &error);
      break;
    case ACTION_SESSION_REJECT:
      valid = OnRejectMessage(msg, &error);
      break;
    case ACTION_SESSION_TERMINATE:
      valid = OnTerminateMessage(msg, &error);
      break;
    case ACTION_TRANSPORT_INFO:
      valid = OnTransportInfoMessage(msg, &error);
      break;
    default:
      error.SetType(buzz::QN_STANZA_BAD_REQUEST);
      error.SetText("unknown session message type");
      valid = false;
  }

  if (valid) {
    SendAcknowledgementMessage(msg.stanza);
  } else {
    SignalErrorMessage(this, msg.stanza, error.type,
                       "modify", error.text, NULL);
  }
}

void PPSession::OnInitiateAcked() {
    // TODO: This is to work around server re-ordering
    // messages.  We send the candidates once the session-initiate
    // is acked.  Once we have fixed the server to guarantee message
    // order, we can remove this case.
  if (!initiate_acked_) {
    initiate_acked_ = true;
    SessionError error;
    SendAllUnsentTransportInfoMessages(&error);
  }
}

bool PPSession::OnInitiateMessage(const SessionMessage& msg,
                                MessageError* error) {
  if (!CheckState(STATE_INIT, error))
    return false;

  SessionInitiate init;
  // msg ==> init FIXME

  SessionError session_error;
  if (!CreateTransportProxies(init.transports, &session_error)) {
    error->SetType(buzz::QN_STANZA_NOT_ACCEPTABLE);
    error->SetText(session_error.text);
    return false;
  }

  set_remote_description(new SessionDescription(init.ClearContents(),
                                                init.groups));
  SetState(STATE_RECEIVEDINITIATE);

  // Users of Session may listen to state change and call Reject().
  if (state() != STATE_SENTREJECT) {
    if (!OnRemoteCandidates(init.transports, error))
      return false;
  }
  return true;
}

bool PPSession::OnAcceptMessage(const SessionMessage& msg, MessageError* error) {
  if (!CheckState(STATE_SENTINITIATE, error))
    return false;

  SessionAccept accept;
  // msg ==> accept FIXME

  // If we get an accept, we can assume the initiate has been
  // received, even if we haven't gotten an IQ response.
  OnInitiateAcked();

  set_remote_description(new SessionDescription(accept.ClearContents(),
                                                accept.groups));
  MaybeEnableMuxingSupport();  // Enable transport channel mux if supported.
  SetState(STATE_RECEIVEDACCEPT);

  // Users of Session may listen to state change and call Reject().
  if (state() != STATE_SENTREJECT) {
    if (!OnRemoteCandidates(accept.transports, error))
      return false;
  }

  return true;
}

bool PPSession::OnRejectMessage(const SessionMessage& msg, MessageError* error) {
  if (!CheckState(STATE_SENTINITIATE, error))
    return false;

  SetState(STATE_RECEIVEDREJECT);
  return true;
}

bool PPSession::OnInfoMessage(const SessionMessage& msg) {
  SignalInfoMessage(this, msg.action_elem);
  return true;
}

bool PPSession::OnTerminateMessage(const SessionMessage& msg,
                                 MessageError* error) {
  SessionTerminate term;
  if (!ParseSessionTerminate(msg.protocol, msg.action_elem, &term, error))
    return false;

  SignalReceivedTerminateReason(this, term.reason);
  if (term.debug_reason != buzz::STR_EMPTY) {
    LOG(LS_VERBOSE) << "Received error on call: " << term.debug_reason;
  }

  SetState(STATE_RECEIVEDTERMINATE);
  return true;
}

bool PPSession::OnTransportInfoMessage(const SessionMessage& msg,
                                     MessageError* error) {
  TransportInfos tinfos;
  // msg ==> tinfos  FIXME  

  if (!OnRemoteCandidates(tinfos, error))
    return false;

  return true;
}

bool BareJidsEqual(const std::string& name1,
                   const std::string& name2) {
  buzz::Jid jid1(name1);
  buzz::Jid jid2(name2);

  return jid1.IsValid() && jid2.IsValid() && jid1.BareEquals(jid2);
}

void PPSession::SetError(Error error) {
  BaseSession::SetError(error);
  if (error != ERROR_NONE)
    signaling_thread()->Post(this, MSG_ERROR);
}


bool PPSession::SendInitiateMessage(const SessionDescription* sdesc,
                                  SessionError* error) {
  SessionInitiate init;
  init.contents = sdesc->contents();
  init.transports = GetEmptyTransportInfos(init.contents);
  init.groups = sdesc->groups();
  //return SendMessage(ACTION_SESSION_INITIATE, init, error);
  return true;     // FIXME
}

bool PPSession::SendAcceptMessage(const SessionDescription* sdesc,
                                SessionError* error) {
  //XmlElements elems;
  //return SendMessage(ACTION_SESSION_ACCEPT, elems, error);
  return true;      // FIXME
}

bool PPSession::SendRejectMessage(const std::string& reason,
                                SessionError* error) {
  SessionTerminate term(reason);
  //return SendMessage(ACTION_SESSION_REJECT, term, error);
  return true;      // FIXME
}

bool PPSession::SendTerminateMessage(const std::string& reason,
                                   SessionError* error) {
  SessionTerminate term(reason);
  //return SendMessage(ACTION_SESSION_TERMINATE, term, error);
  return true;      // FIXME
}

bool PPSession::SendTransportInfoMessage(const TransportInfo& tinfo,
                                       SessionError* error) {
  //return SendMessage(ACTION_TRANSPORT_INFO, tinfo, error);
  return true;      // FIXME
}

bool PPSession::SendTransportInfoMessage(const TransportProxy* transproxy,
                                       const Candidates& candidates,
                                       SessionError* error) {
  return SendTransportInfoMessage(TransportInfo(transproxy->content_name(),
                                                transproxy->type(),
                                                candidates),
                                  error);
}

bool PPSession::SendAllUnsentTransportInfoMessages(SessionError* error) {
  for (TransportMap::const_iterator iter = transport_proxies().begin();
       iter != transport_proxies().end(); ++iter) {
    TransportProxy* transproxy = iter->second;
    if (transproxy->unsent_candidates().size() > 0) {
      if (!SendTransportInfoMessage(
              transproxy, transproxy->unsent_candidates(), error)) {
        LOG(LS_ERROR) << "Could not send unsent transport info messages: "
                      << error->text;
        return false;
      }
      transproxy->ClearUnsentCandidates();
    }
  }
  return true;
}

bool PPSession::SendMessage(ActionType type, const XmlElements& action_elems,
                          SessionError* error) {
  talk_base::scoped_ptr<buzz::XmlElement> stanza(
      new buzz::XmlElement(buzz::QN_IQ));

  //SessionMessage msg(current_protocol_, type, id(), initiator_name());
  //msg.to = remote_name();
  //WriteSessionMessage(msg, action_elems, stanza.get());

  SignalOutgoingMessage(this, stanza.get());
  return true;
}

void PPSession::SendAcknowledgementMessage(const buzz::XmlElement* stanza) {
  talk_base::scoped_ptr<buzz::XmlElement> ack(
      new buzz::XmlElement(buzz::QN_IQ));
  //ack->SetAttr(buzz::QN_TO, remote_name());
  ack->SetAttr(buzz::QN_ID, stanza->Attr(buzz::QN_ID));
  ack->SetAttr(buzz::QN_TYPE, "result");

  SignalOutgoingMessage(this, ack.get());
}

bool PPSession::CheckState(State expected, MessageError* error) {
  ASSERT(state() == expected);
  if (state() != expected) {
    error->SetType(buzz::QN_STANZA_NOT_ALLOWED);
    error->SetText("message not allowed in current state");
    return false;
  }
  return true;
}


