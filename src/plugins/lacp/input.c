/*
 * Copyright (c) 2017 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _GNU_SOURCE
#include <vnet/bonding/node.h>
#include <lacp/node.h>
#include <vlib/stats/stats.h>

static int
lacp_packet_scan (vlib_main_t * vm, member_if_t * mif)
{
  lacp_pdu_t *lacpdu = (lacp_pdu_t *) mif->last_rx_pkt;

  if (lacpdu->subtype != LACP_SUBTYPE)
    return LACP_ERROR_UNSUPPORTED;

  /*
   * According to the spec, no checking on the version number and tlv types.
   * But we may check the tlv lengths.
   */
  if ((lacpdu->actor.tlv_length != sizeof (lacp_actor_partner_t)) ||
      (lacpdu->partner.tlv_length != sizeof (lacp_actor_partner_t)) ||
      (lacpdu->collector.tlv_length != sizeof (lacp_collector_t)) ||
      (lacpdu->terminator.tlv_length != 0))
    return (LACP_ERROR_BAD_TLV);

  return lacp_machine_dispatch (&lacp_rx_machine, vm, mif,
				LACP_RX_EVENT_PDU_RECEIVED, &mif->rx_state);
}

static void
marker_fill_pdu (marker_pdu_t * marker, member_if_t * mif)
{
  marker_pdu_t *pkt = (marker_pdu_t *) mif->last_marker_pkt;

  marker->marker_info = pkt->marker_info;
  marker->marker_info.tlv_type = MARKER_RESPONSE_INFORMATION;
}

void
marker_fill_request_pdu (marker_pdu_t * marker, member_if_t * mif)
{
  marker->marker_info.tlv_type = MARKER_INFORMATION;
  marker->marker_info.requester_port = mif->actor.port_number;
  clib_memcpy (marker->marker_info.requester_system, mif->actor.system, 6);
  marker->marker_info.requester_transaction_id = mif->marker_tx_id;
  mif->marker_tx_id++;
}

static void
send_ethernet_marker_response_pdu (vlib_main_t * vm, member_if_t * mif)
{
  lacp_main_t *lm = &lacp_main;
  u32 *to_next;
  ethernet_marker_pdu_t *h0;
  vnet_hw_interface_t *hw;
  u32 bi0;
  vlib_buffer_t *b0;
  vlib_frame_t *f;
  vnet_main_t *vnm = lm->vnet_main;

  /*
   * see lacp_periodic_init() to understand what's already painted
   * into the buffer by the packet template mechanism
   */
  h0 = vlib_packet_template_get_packet
    (vm, &lm->marker_packet_templates[mif->packet_template_index], &bi0);

  if (!h0)
    return;

  /* Add the interface's ethernet source address */
  hw = vnet_get_sup_hw_interface (vnm, mif->sw_if_index);

  clib_memcpy (h0->ethernet.src_address, hw->hw_address,
	       vec_len (hw->hw_address));

  marker_fill_pdu (&h0->marker, mif);

  /* Set the outbound packet length */
  b0 = vlib_get_buffer (vm, bi0);
  b0->current_length = sizeof (ethernet_marker_pdu_t);
  b0->current_data = 0;
  b0->total_length_not_including_first_buffer = 0;

  /* And the outbound interface */
  vnet_buffer (b0)->sw_if_index[VLIB_TX] = hw->sw_if_index;

  /* And output the packet on the correct interface */
  f = vlib_get_frame_to_node (vm, hw->output_node_index);

  to_next = vlib_frame_vector_args (f);
  to_next[0] = bi0;
  f->n_vectors = 1;

  vlib_put_frame_to_node (vm, hw->output_node_index, f);
  mif->last_marker_pdu_sent_time = vlib_time_now (vm);
  mif->marker_pdu_sent++;
}

static int
handle_marker_protocol (vlib_main_t * vm, member_if_t * mif)
{
  marker_pdu_t *marker = (marker_pdu_t *) mif->last_marker_pkt;

  /*
   * According to the spec, no checking on the version number and tlv types.
   * But we may check the tlv lengths.
   */
  if ((marker->marker_info.tlv_length != sizeof (marker_information_t)) ||
      (marker->terminator.tlv_length != 0))
    return (LACP_ERROR_BAD_TLV);

  send_ethernet_marker_response_pdu (vm, mif);

  return LACP_ERROR_NONE;
}

/*
 * lacp input routine
 */
lacp_error_t
lacp_input (vlib_main_t * vm, vlib_buffer_t * b0, u32 bi0)
{
  bond_main_t *bm = &bond_main;
  member_if_t *mif;
  uword nbytes;
  lacp_error_t e;
  marker_pdu_t *marker;
  uword last_packet_signature;
  bond_if_t *bif;

  mif =
    bond_get_member_by_sw_if_index (vnet_buffer (b0)->sw_if_index[VLIB_RX]);
  if ((mif == 0) || (mif->mode != BOND_MODE_LACP))
    {
      return LACP_ERROR_DISABLED;
    }

  /* Handle marker protocol */
  marker = (marker_pdu_t *) (b0->data + b0->current_data);
  if (marker->subtype == MARKER_SUBTYPE)
    {
      mif->last_marker_pdu_recd_time = vlib_time_now (vm);
      if (mif->last_marker_pkt)
	_vec_len (mif->last_marker_pkt) = 0;
      vec_validate (mif->last_marker_pkt,
		    vlib_buffer_length_in_chain (vm, b0) - 1);
      nbytes = vlib_buffer_contents (vm, bi0, mif->last_marker_pkt);
      ASSERT (nbytes <= vec_len (mif->last_marker_pkt));
      if (nbytes < sizeof (lacp_pdu_t))
	{
	  mif->marker_bad_pdu_received++;
	  return LACP_ERROR_TOO_SMALL;
	}
      e = handle_marker_protocol (vm, mif);
      mif->marker_pdu_received++;
      return e;
    }

  /*
   * typical clib idiom. Don't repeatedly allocate and free
   * the per-neighbor rx buffer. Reset its apparent length to zero
   * and reuse it.
   */
  if (mif->last_rx_pkt)
    _vec_len (mif->last_rx_pkt) = 0;

  /*
   * Make sure the per-neighbor rx buffer is big enough to hold
   * the data we're about to copy
   */
  vec_validate (mif->last_rx_pkt, vlib_buffer_length_in_chain (vm, b0) - 1);

  /*
   * Coalesce / copy the buffer chain into the per-neighbor
   * rx buffer
   */
  nbytes = vlib_buffer_contents (vm, bi0, mif->last_rx_pkt);
  ASSERT (nbytes <= vec_len (mif->last_rx_pkt));

  mif->last_lacpdu_recd_time = vlib_time_now (vm);
  if (nbytes < sizeof (lacp_pdu_t))
    {
      mif->bad_pdu_received++;
      return LACP_ERROR_TOO_SMALL;
    }

  last_packet_signature =
    hash_memory (mif->last_rx_pkt, vec_len (mif->last_rx_pkt), 0xd00b);

  if (mif->last_packet_signature_valid &&
      (mif->last_packet_signature == last_packet_signature) &&
      ((mif->actor.state & LACP_STEADY_STATE) == LACP_STEADY_STATE))
    {
      lacp_start_current_while_timer (vm, mif, mif->ttl_in_seconds);
      e = LACP_ERROR_CACHE_HIT;
    }
  else
    {
      /* Actually scan the packet */
      e = lacp_packet_scan (vm, mif);
      bif = bond_get_bond_if_by_dev_instance (mif->bif_dev_instance);
      vlib_stats_set_gauge (
	bm->stats[bif->sw_if_index][mif->sw_if_index].actor_state,
	mif->actor.state);
      vlib_stats_set_gauge (
	bm->stats[bif->sw_if_index][mif->sw_if_index].partner_state,
	mif->partner.state);
      mif->last_packet_signature_valid = 1;
      mif->last_packet_signature = last_packet_signature;
    }
  mif->pdu_received++;

  if (mif->last_rx_pkt)
    _vec_len (mif->last_rx_pkt) = 0;

  return e;
}

/*
 * setup neighbor hash table
 */
static clib_error_t *
lacp_init (vlib_main_t * vm)
{
  return 0;
}

/* *INDENT-OFF* */
VLIB_INIT_FUNCTION (lacp_init) =
{
  .runs_after = VLIB_INITS("lacp_periodic_init"),
};
/* *INDENT-ON* */

/*
 * packet trace format function, very similar to
 * lacp_packet_scan except that we call the per TLV format
 * functions instead of the per TLV processing functions
 */
u8 *
lacp_input_format_trace (u8 * s, va_list * args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  lacp_input_trace_t *t = va_arg (*args, lacp_input_trace_t *);
  lacp_pdu_t *lacpdu = &t->pkt.lacpdu;
  marker_pdu_t *marker = &t->pkt.marker;
  int i, len;
  u8 *p;
  lacp_state_struct *state_entry;

  s = format (s, "%U:\n", format_vnet_sw_if_index_name, vnet_get_main (),
	      t->sw_if_index);
  s = format (s, "Length: %d\n", t->len);
  if (t->len >= sizeof (lacp_pdu_t))
    {
      switch (lacpdu->subtype)
	{
	case MARKER_SUBTYPE:
	  if (marker->version_number == MARKER_PROTOCOL_VERSION)
	    s = format (s, "  Markerv1\n");
	  else
	    s = format (s, "  Subtype %u, Version %u\n", marker->subtype,
			marker->version_number);
	  s = format (s, "  Marker Information TLV: type %u\n",
		      marker->marker_info.tlv_type);
	  s = format (s, "  Marker Information TLV: length %u\n",
		      marker->marker_info.tlv_length);
	  s = format (s, "  Requester port: %u\n",
		      ntohs (marker->marker_info.requester_port));
	  s = format (s, "  Requester system: %U\n", format_ethernet_address,
		      marker->marker_info.requester_system);
	  s = format (s, "  Requester transaction ID: %u\n",
		      ntohl (marker->marker_info.requester_transaction_id));
	  break;

	case LACP_SUBTYPE:
	  if (lacpdu->version_number == LACP_ACTOR_LACP_VERSION)
	    s = format (s, "  LACPv1\n");
	  else
	    s = format (s, "  Subtype %u, Version %u\n", lacpdu->subtype,
			lacpdu->version_number);
	  s = format (s, "  Actor Information TLV: length %u\n",
		      lacpdu->actor.tlv_length);
	  s = format (s, "    System %U\n", format_ethernet_address,
		      lacpdu->actor.port_info.system);
	  s = format (s, "    System priority %u\n",
		      ntohs (lacpdu->actor.port_info.system_priority));
	  s = format (s, "    Key %u\n", ntohs (lacpdu->actor.port_info.key));
	  s = format (s, "    Port priority %u\n",
		      ntohs (lacpdu->actor.port_info.port_priority));
	  s = format (s, "    Port number %u\n",
		      ntohs (lacpdu->actor.port_info.port_number));
	  s = format (s, "    State 0x%x\n", lacpdu->actor.port_info.state);
	  state_entry = (lacp_state_struct *) & lacp_state_array;
	  while (state_entry->str)
	    {
	      if (lacpdu->actor.port_info.state & (1 << state_entry->bit))
		s = format (s, "      %s (%d)\n", state_entry->str,
			    state_entry->bit);
	      state_entry++;
	    }

	  s = format (s, "  Partner Information TLV: length %u\n",
		      lacpdu->partner.tlv_length);
	  s = format (s, "    System %U\n", format_ethernet_address,
		      lacpdu->partner.port_info.system);
	  s = format (s, "    System priority %u\n",
		      ntohs (lacpdu->partner.port_info.system_priority));
	  s =
	    format (s, "    Key %u\n", ntohs (lacpdu->partner.port_info.key));
	  s =
	    format (s, "    Port priority %u\n",
		    ntohs (lacpdu->partner.port_info.port_priority));
	  s =
	    format (s, "    Port number %u\n",
		    ntohs (lacpdu->partner.port_info.port_number));
	  s = format (s, "    State 0x%x\n", lacpdu->partner.port_info.state);
	  state_entry = (lacp_state_struct *) & lacp_state_array;
	  while (state_entry->str)
	    {
	      if (lacpdu->partner.port_info.state & (1 << state_entry->bit))
		s = format (s, "      %s (%d)\n", state_entry->str,
			    state_entry->bit);
	      state_entry++;
	    }
	  break;

	default:
	  break;
	}
    }

  if (t->len > sizeof (lacp_pdu_t))
    len = sizeof (lacp_pdu_t);
  else
    len = t->len;
  p = (u8 *) lacpdu;
  for (i = 0; i < len; i++)
    {
      if ((i % 16) == 0)
	{
	  if (i)
	    s = format (s, "\n");
	  s = format (s, "  0x%04x: ", i);
	}
      if ((i % 2) == 0)
	s = format (s, " ");
      s = format (s, "%02x", p[i]);
    }

  return s;
}

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
