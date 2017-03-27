/**
 * @file rpc_sm_api.cc
 * @brief Rpc session functions that are exposed to the user.
 */

#include <algorithm>

#include "rpc.h"
#include "util/udp_client.h"

namespace ERpc {

/*
 * This function is not on the critical path and is exposed to the user,
 * so the args checking is always enabled (i.e., no asserts).
 */
template <class TTr>
Session *Rpc<TTr>::create_session(const char *rem_hostname, uint8_t rem_app_tid,
                                  uint8_t rem_phy_port) {
  /* Create the basic issue message */
  char issue_msg[kMaxIssueMsgLen];
  sprintf(issue_msg, "eRPC Rpc %u: create_session() failed. Issue", app_tid);

  /* Check that the caller is the creator thread */
  if (!in_creator()) {
    erpc_dprintf("%s: Caller thread is not the creator thread.\n", issue_msg);
    return nullptr;
  }

  /* Check remote fabric port */
  if (rem_phy_port >= kMaxPhyPorts) {
    erpc_dprintf("%s: Invalid remote fabric port %u.\n", issue_msg,
                 rem_phy_port);
    return nullptr;
  }

  /* Check remote hostname */
  if (rem_hostname == nullptr || strlen(rem_hostname) > kMaxHostnameLen) {
    erpc_dprintf("%s: Invalid remote hostname.\n", issue_msg);
    return nullptr;
  }

  /* Creating a session to one's own Rpc as the client is not allowed */
  if (strcmp(rem_hostname, nexus->hostname.c_str()) == 0 &&
      rem_app_tid == app_tid) {
    erpc_dprintf("%s: Remote Rpc is same as local.\n", issue_msg);
    return nullptr;
  }

  /* Creating two sessions as client to the same remote Rpc is not allowed */
  for (Session *existing_session : session_vec) {
    if (existing_session == nullptr) {
      continue;
    }

    if (strcmp(existing_session->server.hostname, rem_hostname) == 0 &&
        existing_session->server.app_tid == rem_app_tid) {
      /*
       * existing_session->server != this Rpc, since existing_session->server
       * matches (rem_hostname, rem_app_tid), which does match this
       * Rpc (checked earlier). So we must be the client.
       */
      assert(existing_session->is_client());
      erpc_dprintf("%s: Session to %s already exists.\n", issue_msg,
                   existing_session->server.rpc_name().c_str());
      return nullptr;
    }
  }

  /* Ensure bounded session_vec size */
  if (session_vec.size() >= kMaxSessionsPerThread) {
    erpc_dprintf("%s: Session limit (%zu) reached.\n", issue_msg,
                 kMaxSessionsPerThread);
    return nullptr;
  }

  /* Create a new session and fill prealloc MsgBuffers. XXX: Use pool? */
  Session *session =
      new Session(Session::Role::kClient, SessionState::kConnectInProgress);

  /* Fill prealloc response MsgBuffers for the client session */
  for (size_t i = 0; i < Session::kSessionReqWindow; i++) {
    MsgBuffer &resp_msgbuf_i = session->sslot_arr[i].pre_resp_msgbuf;
    resp_msgbuf_i = alloc_msg_buffer(TTr::kMaxDataPerPkt);

    if (resp_msgbuf_i.buf == nullptr) {
      /*
       * We haven't assigned a session number or allocated non-prealloc
       * MsgBuffers yet, so just free prealloc MsgBuffers 0 -- (i - 1).
       */
      for (size_t j = 0; j < i; j++) {
        MsgBuffer &resp_msgbuf_j = session->sslot_arr[j].pre_resp_msgbuf;
        assert(resp_msgbuf_j.buf != nullptr);
        free_msg_buffer(resp_msgbuf_j);
      }

      erpc_dprintf("%s: Failed to allocate prealloc MsgBuffer.\n", issue_msg);
      return nullptr;
    }
  }

  /*
   * Fill in client and server endpoint metadata. Commented server fields will
   * be filled when the connect response is received.
   */
  SessionEndpoint &client_endpoint = session->client;

  client_endpoint.transport_type = transport->transport_type;
  strcpy((char *)client_endpoint.hostname, nexus->hostname.c_str());
  client_endpoint.phy_port = phy_port;
  client_endpoint.app_tid = app_tid;
  client_endpoint.session_num = session_vec.size();
  client_endpoint.secret = slow_rand.next_u64() & ((1ull << kSecretBits) - 1);
  transport->fill_local_routing_info(&client_endpoint.routing_info);

  SessionEndpoint &server_endpoint = session->server;
  server_endpoint.transport_type = transport->transport_type;
  strcpy((char *)server_endpoint.hostname, rem_hostname);
  server_endpoint.phy_port = rem_phy_port;
  server_endpoint.app_tid = rem_app_tid;
  // server_endpoint.session_num = ??
  server_endpoint.secret = client_endpoint.secret; /* Secret is shared */
  // server_endpoint.routing_info = ??

  session->local_session_num = client_endpoint.session_num;

  session_vec.push_back(session); /* Add to list of all sessions */
  mgmt_retry_queue_add(session);  /* Record management request for retry */

  erpc_dprintf(
      "eRPC Rpc %u: Sending first session connect req for session %u to %s.\n",
      app_tid, client_endpoint.session_num, rem_hostname);
  send_connect_req_one(session);

  return session;
}

template <class TTr>
bool Rpc<TTr>::destroy_session(Session *session) {
  /* Check that the caller is the creator thread */
  if (!in_creator()) {
    erpc_dprintf(
        "eRPC Rpc %u: destroy_session() failed: Caller thread is not creator\n",
        app_tid);
    return false;
  }

  if (session == nullptr || !session->is_client()) {
    erpc_dprintf("eRPC Rpc %u: destroy_session() failed: Invalid session.\n",
                 app_tid);
    return false;
  }

  uint16_t session_num = session->client.session_num;
  char issue_msg[kMaxIssueMsgLen];
  sprintf(issue_msg,
          "eRPC Rpc %u: destroy_session() failed for session %u. Issue",
          app_tid, session_num);

  switch (session->state) {
    case SessionState::kConnectInProgress:
      /* Can't disconnect right now. User needs to wait. */
      assert(mgmt_retry_queue_contains(session));
      erpc_dprintf("%s: Session connection in progress.\n", issue_msg);
      return false;

    case SessionState::kConnected:
      session->state = SessionState::kDisconnectInProgress;
      mgmt_retry_queue_add(session); /* Checks that session is not in flight */

      erpc_dprintf(
          "eRPC Rpc %u: Sending first session disconnect req for session %u.\n",
          app_tid, session->local_session_num);
      send_disconnect_req_one(session);
      return true;

    case SessionState::kDisconnectInProgress:
      assert(mgmt_retry_queue_contains(session));
      erpc_dprintf("%s: Session disconnection in progress.\n", issue_msg);
      return false;

    case SessionState::kDisconnected:
      assert(!mgmt_retry_queue_contains(session));
      erpc_dprintf_noargs(
          "eRPC Rpc: destroy_session() failed. Issue: "
          "Session already destroyed.\n");
      return false;
  }
  exit(-1);
  return false;
}

}  // End ERpc
