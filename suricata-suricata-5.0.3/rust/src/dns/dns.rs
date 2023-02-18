/* Copyright (C) 2017 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

extern crate nom;

use std;
use std::mem::transmute;

use crate::log::*;
use crate::applayer::LoggerFlags;
use crate::core;
use crate::dns::parser;

/// DNS record types.
pub const DNS_RECORD_TYPE_A           : u16 = 1;
pub const DNS_RECORD_TYPE_NS          : u16 = 2;
pub const DNS_RECORD_TYPE_MD          : u16 = 3;   // Obsolete
pub const DNS_RECORD_TYPE_MF          : u16 = 4;   // Obsolete
pub const DNS_RECORD_TYPE_CNAME       : u16 = 5;
pub const DNS_RECORD_TYPE_SOA         : u16 = 6;
pub const DNS_RECORD_TYPE_MB          : u16 = 7;   // Experimental
pub const DNS_RECORD_TYPE_MG          : u16 = 8;   // Experimental
pub const DNS_RECORD_TYPE_MR          : u16 = 9;   // Experimental
pub const DNS_RECORD_TYPE_NULL        : u16 = 10;  // Experimental
pub const DNS_RECORD_TYPE_WKS         : u16 = 11;
pub const DNS_RECORD_TYPE_PTR         : u16 = 12;
pub const DNS_RECORD_TYPE_HINFO       : u16 = 13;
pub const DNS_RECORD_TYPE_MINFO       : u16 = 14;
pub const DNS_RECORD_TYPE_MX          : u16 = 15;
pub const DNS_RECORD_TYPE_TXT         : u16 = 16;
pub const DNS_RECORD_TYPE_RP          : u16 = 17;
pub const DNS_RECORD_TYPE_AFSDB       : u16 = 18;
pub const DNS_RECORD_TYPE_X25         : u16 = 19;
pub const DNS_RECORD_TYPE_ISDN        : u16 = 20;
pub const DNS_RECORD_TYPE_RT          : u16 = 21;
pub const DNS_RECORD_TYPE_NSAP        : u16 = 22;
pub const DNS_RECORD_TYPE_NSAPPTR     : u16 = 23;
pub const DNS_RECORD_TYPE_SIG         : u16 = 24;
pub const DNS_RECORD_TYPE_KEY         : u16 = 25;
pub const DNS_RECORD_TYPE_PX          : u16 = 26;
pub const DNS_RECORD_TYPE_GPOS        : u16 = 27;
pub const DNS_RECORD_TYPE_AAAA        : u16 = 28;
pub const DNS_RECORD_TYPE_LOC         : u16 = 29;
pub const DNS_RECORD_TYPE_NXT         : u16 = 30;  // Obsolete
pub const DNS_RECORD_TYPE_SRV         : u16 = 33;
pub const DNS_RECORD_TYPE_ATMA        : u16 = 34;
pub const DNS_RECORD_TYPE_NAPTR       : u16 = 35;
pub const DNS_RECORD_TYPE_KX          : u16 = 36;
pub const DNS_RECORD_TYPE_CERT        : u16 = 37;
pub const DNS_RECORD_TYPE_A6          : u16 = 38;  // Obsolete
pub const DNS_RECORD_TYPE_DNAME       : u16 = 39;
pub const DNS_RECORD_TYPE_OPT         : u16 = 41;
pub const DNS_RECORD_TYPE_APL         : u16 = 42;
pub const DNS_RECORD_TYPE_DS          : u16 = 43;
pub const DNS_RECORD_TYPE_SSHFP       : u16 = 44;
pub const DNS_RECORD_TYPE_IPSECKEY    : u16 = 45;
pub const DNS_RECORD_TYPE_RRSIG       : u16 = 46;
pub const DNS_RECORD_TYPE_NSEC        : u16 = 47;
pub const DNS_RECORD_TYPE_DNSKEY      : u16 = 48;
pub const DNS_RECORD_TYPE_DHCID       : u16 = 49;
pub const DNS_RECORD_TYPE_NSEC3       : u16 = 50;
pub const DNS_RECORD_TYPE_NSEC3PARAM  : u16 = 51;
pub const DNS_RECORD_TYPE_TLSA        : u16 = 52;
pub const DNS_RECORD_TYPE_HIP         : u16 = 55;
pub const DNS_RECORD_TYPE_CDS         : u16 = 59;
pub const DNS_RECORD_TYPE_CDNSKEY     : u16 = 60;
pub const DNS_RECORD_TYPE_SPF         : u16 = 99;  // Obsolete
pub const DNS_RECORD_TYPE_TKEY        : u16 = 249;
pub const DNS_RECORD_TYPE_TSIG        : u16 = 250;
pub const DNS_RECORD_TYPE_MAILA       : u16 = 254; // Obsolete
pub const DNS_RECORD_TYPE_ANY         : u16 = 255;
pub const DNS_RECORD_TYPE_URI         : u16 = 256;

/// DNS error codes.
pub const DNS_RCODE_NOERROR:  u16 = 0;
pub const DNS_RCODE_FORMERR:  u16 = 1;
pub const DNS_RCODE_SERVFAIL: u16 = 2;
pub const DNS_RCODE_NXDOMAIN: u16 = 3;
pub const DNS_RCODE_NOTIMP:   u16 = 4;
pub const DNS_RCODE_REFUSED:  u16 = 5;
pub const DNS_RCODE_YXDOMAIN: u16 = 6;
pub const DNS_RCODE_YXRRSET:  u16 = 7;
pub const DNS_RCODE_NXRRSET:  u16 = 8;
pub const DNS_RCODE_NOTAUTH:  u16 = 9;
pub const DNS_RCODE_NOTZONE:  u16 = 10;
// Support for OPT RR from RFC6891 will be needed to
// parse RCODE values over 15
pub const DNS_RCODE_BADVERS:  u16 = 16;
pub const DNS_RCODE_BADSIG:   u16 = 16;
pub const DNS_RCODE_BADKEY:   u16 = 17;
pub const DNS_RCODE_BADTIME:  u16 = 18;
pub const DNS_RCODE_BADMODE:  u16 = 19;
pub const DNS_RCODE_BADNAME:  u16 = 20;
pub const DNS_RCODE_BADALG:   u16 = 21;
pub const DNS_RCODE_BADTRUNC: u16 = 22;


/// The maximum number of transactions to keep in the queue pending
/// processing before they are aggressively purged. Due to the
/// stateless nature of this parser this is rarely needed, especially
/// when one call to parse a request parses and a single request, and
/// likewise for responses.
///
/// Where this matters is when one TCP buffer contains multiple
/// requests are responses and one call into the parser creates
/// multiple transactions. In this case we have to hold onto
/// transactions longer than until handling the next transaction so it
/// gets logged.
const MAX_TRANSACTIONS: usize = 32;

#[repr(u32)]
pub enum DNSEvent {
    UnsolicitedResponse = 0,
    MalformedData,
    NotRequest,
    NotResponse,
    ZFlagSet,
    Flooded,
    StateMemCapReached,
}

#[derive(Debug,PartialEq)]
pub struct DNSHeader {
    pub tx_id: u16,
    pub flags: u16,
    pub questions: u16,
    pub answer_rr: u16,
    pub authority_rr: u16,
    pub additional_rr: u16,
}

#[derive(Debug)]
pub struct DNSQueryEntry {
    pub name: Vec<u8>,
    pub rrtype: u16,
    pub rrclass: u16,
}

#[derive(Debug,PartialEq)]
pub struct DNSAnswerEntry {
    pub name: Vec<u8>,
    pub rrtype: u16,
    pub rrclass: u16,
    pub ttl: u32,
    pub data: Vec<u8>,
}

#[derive(Debug)]
pub struct DNSRequest {
    pub header: DNSHeader,
    pub queries: Vec<DNSQueryEntry>,
}

#[derive(Debug)]
pub struct DNSResponse {
    pub header: DNSHeader,
    pub queries: Vec<DNSQueryEntry>,
    pub answers: Vec<DNSAnswerEntry>,
    pub authorities: Vec<DNSAnswerEntry>,
}

#[derive(Debug)]
pub struct DNSTransaction {
    pub id: u64,
    pub request: Option<DNSRequest>,
    pub response: Option<DNSResponse>,
    detect_flags_ts: u64,
    detect_flags_tc: u64,
    pub logged: LoggerFlags,
    pub de_state: Option<*mut core::DetectEngineState>,
    pub events: *mut core::AppLayerDecoderEvents,
}

impl DNSTransaction {

    pub fn new() -> DNSTransaction {
        return DNSTransaction{
            id: 0,
            request: None,
            response: None,
            detect_flags_ts: 0,
            detect_flags_tc: 0,
            logged: LoggerFlags::new(),
            de_state: None,
            events: std::ptr::null_mut(),
        }
    }

    pub fn free(&mut self) {
        if self.events != std::ptr::null_mut() {
            core::sc_app_layer_decoder_events_free_events(&mut self.events);
        }
        match self.de_state {
            Some(state) => {
                core::sc_detect_engine_state_free(state);
            }
            None => { },
        }
    }

    /// Get the DNS transactions ID (not the internal tracking ID).
    pub fn tx_id(&self) -> u16 {
        if let &Some(ref request) = &self.request {
            return request.header.tx_id;
        }
        if let &Some(ref response) = &self.response {
            return response.header.tx_id;
        }

        // Shouldn't happen.
        return 0;
    }

    /// Get the reply code of the transaction. Note that this will
    /// also return 0 if there is no reply.
    pub fn rcode(&self) -> u16 {
        if let &Some(ref response) = &self.response {
            return response.header.flags & 0x000f;
        }
        return 0;
    }

}

impl Drop for DNSTransaction {
    fn drop(&mut self) {
        self.free();
    }
}

pub struct DNSState {
    // Internal transaction ID.
    pub tx_id: u64,

    // Transactions.
    pub transactions: Vec<DNSTransaction>,

    pub events: u16,

    pub request_buffer: Vec<u8>,
    pub response_buffer: Vec<u8>,

    gap: bool,
}

impl DNSState {

    pub fn new() -> DNSState {
        return DNSState{
            tx_id: 0,
            transactions: Vec::new(),
            events: 0,
            request_buffer: Vec::new(),
            response_buffer: Vec::new(),
            gap: false,
        };
    }

    /// Allocate a new state with capacites in the buffers for
    /// potentially buffering as might be needed in TCP.
    pub fn new_tcp() -> DNSState {
        return DNSState{
            tx_id: 0,
            transactions: Vec::new(),
            events: 0,
            request_buffer: Vec::with_capacity(0xffff),
            response_buffer: Vec::with_capacity(0xffff),
            gap: false,
        };
    }

    pub fn new_tx(&mut self) -> DNSTransaction {
        let mut tx = DNSTransaction::new();
        self.tx_id += 1;
        tx.id = self.tx_id;
        return tx;
    }

    pub fn free_tx(&mut self, tx_id: u64) {
        let len = self.transactions.len();
        let mut found = false;
        let mut index = 0;
        for i in 0..len {
            let tx = &self.transactions[i];
            if tx.id == tx_id + 1 {
                found = true;
                index = i;
                break;
            }
        }
        if found {
            self.transactions.remove(index);
        }
    }

    // Purges all transactions except one. This is a stateless parser
    // so we don't need to hang onto old transactions.
    //
    // This is to actually handle an edge case where a DNS flood
    // occurs in a single direction with no response packets. In such
    // a case the functions to free a transaction are never called by
    // the app-layer as they require bidirectional traffic.
    pub fn purge(&mut self, tx_id: u64) {
        while self.transactions.len() > MAX_TRANSACTIONS {
            if self.transactions[0].id == tx_id + 1 {
                return;
            }
            SCLogDebug!("Purging DNS TX with ID {}", self.transactions[0].id);
            self.transactions.remove(0);
        }
    }

    pub fn get_tx(&mut self, tx_id: u64) -> Option<&DNSTransaction> {
        SCLogDebug!("get_tx: tx_id={}", tx_id);
        self.purge(tx_id);
        for tx in &mut self.transactions {
            if tx.id == tx_id + 1 {
                SCLogDebug!("Found DNS TX with ID {}", tx_id);
                return Some(tx);
            }
        }
        SCLogDebug!("Failed to find DNS TX with ID {}", tx_id);
        return None;
    }

    /// Set an event. The event is set on the most recent transaction.
    pub fn set_event(&mut self, event: DNSEvent) {
        let len = self.transactions.len();
        if len == 0 {
            return;
        }

        let tx = &mut self.transactions[len - 1];
        core::sc_app_layer_decoder_events_set_event_raw(&mut tx.events,
                                                        event as u8);
        self.events += 1;
    }

    pub fn parse_request(&mut self, input: &[u8]) -> bool {
        match parser::dns_parse_request(input) {
            Ok((_, request)) => {
                if request.header.flags & 0x8000 != 0 {
                    SCLogDebug!("DNS message is not a request");
                    self.set_event(DNSEvent::NotRequest);
                    return false;
                }

                if request.header.flags & 0x0040 != 0 {
                    SCLogDebug!("Z-flag set on DNS response");
                    self.set_event(DNSEvent::ZFlagSet);
                    return false;
                }

                let mut tx = self.new_tx();
                tx.request = Some(request);
                self.transactions.push(tx);
                return true;
            }
            Err(nom::Err::Incomplete(_)) => {
                // Insufficient data.
                SCLogDebug!("Insufficient data while parsing DNS request");
                self.set_event(DNSEvent::MalformedData);
                return false;
            }
            Err(_) => {
                // Error, probably malformed data.
                SCLogDebug!("An error occurred while parsing DNS request");
                self.set_event(DNSEvent::MalformedData);
                return false;
            }
        }
    }

    pub fn parse_response(&mut self, input: &[u8]) -> bool {
        match parser::dns_parse_response(input) {
            Ok((_, response)) => {

                SCLogDebug!("Response header flags: {}", response.header.flags);

                if response.header.flags & 0x8000 == 0 {
                    SCLogDebug!("DNS message is not a response");
                    self.set_event(DNSEvent::NotResponse);
                }

                if response.header.flags & 0x0040 != 0 {
                    SCLogDebug!("Z-flag set on DNS response");
                    self.set_event(DNSEvent::ZFlagSet);
                    return false;
                }

                let mut tx = self.new_tx();
                tx.response = Some(response);
                self.transactions.push(tx);
                return true;
            }
            Err(nom::Err::Incomplete(_)) => {
                // Insufficient data.
                SCLogDebug!("Insufficient data while parsing DNS response");
                self.set_event(DNSEvent::MalformedData);
                return false;
            }
            Err(_) => {
                // Error, probably malformed data.
                SCLogDebug!("An error occurred while parsing DNS response");
                self.set_event(DNSEvent::MalformedData);
                return false;
            }
        }
    }

    /// TCP variation of response request parser to handle the length
    /// prefix as well as buffering.
    ///
    /// Always buffer and read from the buffer. Should optimize to skip
    /// the buffer if not needed.
    ///
    /// Returns the number of messages parsed.
    pub fn parse_request_tcp(&mut self, input: &[u8]) -> i8 {
        if self.gap {
            let (is_dns, _) = probe_tcp(input);
            if is_dns {
                self.gap = false;
            } else {
                return 0
            }
        }

        self.request_buffer.extend_from_slice(input);

        let mut count = 0;
        while self.request_buffer.len() > 0 {
            let size = match nom::be_u16(&self.request_buffer) {
                Ok((_, len)) => len,
                _ => 0
            } as usize;
            SCLogDebug!("Have {} bytes, need {} to parse",
                        self.request_buffer.len(), size);
            if size > 0 && self.request_buffer.len() >= size + 2 {
                let msg: Vec<u8> = self.request_buffer.drain(0..(size + 2))
                    .collect();
                if self.parse_request(&msg[2..]) {
                    count += 1
                }
            } else {
                SCLogDebug!("Not enough DNS traffic to parse.");
                break;
            }
        }
        return count;
    }

    /// TCP variation of the response parser to handle the length
    /// prefix as well as buffering.
    ///
    /// Always buffer and read from the buffer. Should optimize to skip
    /// the buffer if not needed.
    ///
    /// Returns the number of messages parsed.
    pub fn parse_response_tcp(&mut self, input: &[u8]) -> i8 {
        if self.gap {
            let (is_dns, _) = probe_tcp(input);
            if is_dns {
                self.gap = false;
            } else {
                return 0
            }
        }

        self.response_buffer.extend_from_slice(input);

        let mut count = 0;
        while self.response_buffer.len() > 0 {
            let size = match nom::be_u16(&self.response_buffer) {
                Ok((_, len)) => len,
                _ => 0
            } as usize;
            if size > 0 && self.response_buffer.len() >= size + 2 {
                let msg: Vec<u8> = self.response_buffer.drain(0..(size + 2))
                    .collect();
                if self.parse_response(&msg[2..]) {
                    count += 1;
                }
            } else {
                break;
            }
        }
        return count;
    }

    /// A gap has been seen in the request direction. Set the gap flag
    /// to clear any buffered data.
    pub fn request_gap(&mut self, gap: u32) {
        if gap > 0 {
            self.request_buffer.clear();
            self.gap = true;
        }
    }

    /// A gap has been seen in the response direction. Set the gap
    /// flag to clear any buffered data.
    pub fn response_gap(&mut self, gap: u32) {
        if gap > 0 {
            self.response_buffer.clear();
            self.gap = true;
        }
    }
}

/// Probe input to see if it looks like DNS.
fn probe(input: &[u8]) -> (bool, bool) {
    match parser::dns_parse_request(input) {
        Ok((_, request)) => {
            let is_request = request.header.flags & 0x8000 == 0;
            return (true, is_request);
        },
        Err(_) => (false, false),
    }
}

/// Probe TCP input to see if it looks like DNS.
pub fn probe_tcp(input: &[u8]) -> (bool, bool) {
    match nom::be_u16(input) {
        Ok((rem, _)) => {
            return probe(rem);
        },
        _ => {}
    }
    return (false, false);
}

/// Returns *mut DNSState
#[no_mangle]
pub extern "C" fn rs_dns_state_new() -> *mut std::os::raw::c_void {
    let state = DNSState::new();
    let boxed = Box::new(state);
    return unsafe{transmute(boxed)};
}

/// Returns *mut DNSState
#[no_mangle]
pub extern "C" fn rs_dns_state_tcp_new() -> *mut std::os::raw::c_void {
    let state = DNSState::new_tcp();
    let boxed = Box::new(state);
    return unsafe{transmute(boxed)};
}

/// Params:
/// - state: *mut DNSState as void pointer
#[no_mangle]
pub extern "C" fn rs_dns_state_free(state: *mut std::os::raw::c_void) {
    // Just unbox...
    let _drop: Box<DNSState> = unsafe{transmute(state)};
}

#[no_mangle]
pub extern "C" fn rs_dns_state_tx_free(state: &mut DNSState,
                                       tx_id: u64)
{
    state.free_tx(tx_id);
}

/// C binding parse a DNS request. Returns 1 on success, -1 on failure.
#[no_mangle]
pub extern "C" fn rs_dns_parse_request(_flow: *mut core::Flow,
                                       state: &mut DNSState,
                                       _pstate: *mut std::os::raw::c_void,
                                       input: *const u8,
                                       input_len: u32,
                                       _data: *mut std::os::raw::c_void)
                                       -> i8 {
    let buf = unsafe{std::slice::from_raw_parts(input, input_len as usize)};
    if state.parse_request(buf) {
        1
    } else {
        -1
    }
}

#[no_mangle]
pub extern "C" fn rs_dns_parse_response(_flow: *mut core::Flow,
                                        state: &mut DNSState,
                                        _pstate: *mut std::os::raw::c_void,
                                        input: *const u8,
                                        input_len: u32,
                                        _data: *mut std::os::raw::c_void)
                                        -> i8 {
    let buf = unsafe{std::slice::from_raw_parts(input, input_len as usize)};
    if state.parse_response(buf) {
        1
    } else {
        -1
    }
}

/// C binding parse a DNS request. Returns 1 on success, -1 on failure.
#[no_mangle]
pub extern "C" fn rs_dns_parse_request_tcp(_flow: *mut core::Flow,
                                           state: &mut DNSState,
                                           _pstate: *mut std::os::raw::c_void,
                                           input: *const u8,
                                           input_len: u32,
                                           _data: *mut std::os::raw::c_void)
                                           -> i8 {
    if input_len > 0 {
        if input != std::ptr::null_mut() {
            let buf = unsafe{
                std::slice::from_raw_parts(input, input_len as usize)};
            return state.parse_request_tcp(buf);
        }
        state.request_gap(input_len);
    }
    return 0;
}

#[no_mangle]
pub extern "C" fn rs_dns_parse_response_tcp(_flow: *mut core::Flow,
                                            state: &mut DNSState,
                                            _pstate: *mut std::os::raw::c_void,
                                            input: *const u8,
                                            input_len: u32,
                                            _data: *mut std::os::raw::c_void)
                                            -> i8 {
    if input_len > 0 {
        if input != std::ptr::null_mut() {
            let buf = unsafe{
                std::slice::from_raw_parts(input, input_len as usize)};
            return state.parse_response_tcp(buf);
        }
        state.response_gap(input_len);
    }
    return 0;
}

#[no_mangle]
pub extern "C" fn rs_dns_state_progress_completion_status(
    _direction: u8)
    -> std::os::raw::c_int
{
    SCLogDebug!("rs_dns_state_progress_completion_status");
    return 1;
}

#[no_mangle]
pub extern "C" fn rs_dns_tx_get_alstate_progress(_tx: &mut DNSTransaction,
                                                 _direction: u8)
                                                 -> u8
{
    // This is a stateless parser, just the existence of a transaction
    // means its complete.
    SCLogDebug!("rs_dns_tx_get_alstate_progress");
    return 1;
}

#[no_mangle]
pub extern "C" fn rs_dns_tx_set_detect_flags(tx: &mut DNSTransaction,
                                             dir: u8,
                                             flags: u64)
{
    if dir & core::STREAM_TOSERVER != 0 {
        tx.detect_flags_ts = flags as u64;
    } else {
        tx.detect_flags_tc = flags as u64;
    }
}

#[no_mangle]
pub extern "C" fn rs_dns_tx_get_detect_flags(tx: &mut DNSTransaction,
                                             dir: u8)
                                       -> u64
{
    if dir & core::STREAM_TOSERVER != 0 {
        return tx.detect_flags_ts as u64;
    } else {
        return tx.detect_flags_tc as u64;
    }
}

#[no_mangle]
pub extern "C" fn rs_dns_tx_set_logged(_state: &mut DNSState,
                                       tx: &mut DNSTransaction,
                                       logged: u32)
{
    tx.logged.set(logged);
}

#[no_mangle]
pub extern "C" fn rs_dns_tx_get_logged(_state: &mut DNSState,
                                       tx: &mut DNSTransaction)
                                       -> u32
{
    return tx.logged.get();
}

#[no_mangle]
pub extern "C" fn rs_dns_state_get_tx_count(state: &mut DNSState)
                                            -> u64
{
    SCLogDebug!("rs_dns_state_get_tx_count: returning {}", state.tx_id);
    return state.tx_id;
}

#[no_mangle]
pub extern "C" fn rs_dns_state_get_tx(state: &mut DNSState,
                                      tx_id: u64)
                                      -> *mut DNSTransaction
{
    match state.get_tx(tx_id) {
        Some(tx) => {
            return unsafe{transmute(tx)};
        }
        None => {
            return std::ptr::null_mut();
        }
    }
}

#[no_mangle]
pub extern "C" fn rs_dns_state_set_tx_detect_state(
    tx: &mut DNSTransaction,
    de_state: &mut core::DetectEngineState)
{
    tx.de_state = Some(de_state);
}

#[no_mangle]
pub extern "C" fn rs_dns_state_get_tx_detect_state(
    tx: &mut DNSTransaction)
    -> *mut core::DetectEngineState
{
    match tx.de_state {
        Some(ds) => {
            return ds;
        },
        None => {
            return std::ptr::null_mut();
        }
    }
}

#[no_mangle]
pub extern "C" fn rs_dns_state_get_events(tx: *mut std::os::raw::c_void)
                                          -> *mut core::AppLayerDecoderEvents
{
    let tx = cast_pointer!(tx, DNSTransaction);
    return tx.events;
}

#[no_mangle]
pub extern "C" fn rs_dns_tx_get_query_name(tx: &mut DNSTransaction,
                                       i: u16,
                                       buf: *mut *const u8,
                                       len: *mut u32)
                                       -> u8
{
    if let &Some(ref request) = &tx.request {
        if (i as usize) < request.queries.len() {
            let query = &request.queries[i as usize];
            if query.name.len() > 0 {
                unsafe {
                    *len = query.name.len() as u32;
                    *buf = query.name.as_ptr();
                }
                return 1;
            }
        }
    }
    return 0;
}

/// Get the DNS transaction ID of a transaction.
//
/// extern uint16_t rs_dns_tx_get_tx_id(RSDNSTransaction *);
#[no_mangle]
pub extern "C" fn rs_dns_tx_get_tx_id(tx: &mut DNSTransaction) -> u16
{
    return tx.tx_id()
}

/// Get the DNS response flags for a transaction.
///
/// extern uint16_t rs_dns_tx_get_response_flags(RSDNSTransaction *);
#[no_mangle]
pub extern "C" fn rs_dns_tx_get_response_flags(tx: &mut DNSTransaction)
                                           -> u16
{
    return tx.rcode();
}

#[no_mangle]
pub extern "C" fn rs_dns_tx_get_query_rrtype(tx: &mut DNSTransaction,
                                         i: u16,
                                         rrtype: *mut u16)
                                         -> u8
{
    if let &Some(ref request) = &tx.request {
        if (i as usize) < request.queries.len() {
            let query = &request.queries[i as usize];
            if query.name.len() > 0 {
                unsafe {
                    *rrtype = query.rrtype;
                }
                return 1;
            }
        }
    }
    return 0;
}

#[no_mangle]
pub extern "C" fn rs_dns_probe(input: *const u8, len: u32, rdir: *mut u8)
                               -> u8
{
    let slice: &[u8] = unsafe {
        std::slice::from_raw_parts(input as *mut u8, len as usize)
    };
    let (is_dns, is_request) = probe(slice);
    if is_dns {
        let dir = if is_request {
            core::STREAM_TOSERVER
        } else {
            core::STREAM_TOCLIENT
        };
        unsafe { *rdir = dir };

        return 1;
    }
    return 0;
}

#[no_mangle]
pub extern "C" fn rs_dns_probe_tcp(direction: u8,
                                   input: *const u8,
                                   len: u32,
                                   rdir: *mut u8)
                                   -> u8
{
    let slice: &[u8] = unsafe {
        std::slice::from_raw_parts(input as *mut u8, len as usize)
    };
    let (is_dns, is_request) = probe_tcp(slice);
    if is_dns {
        let dir = if is_request {
            core::STREAM_TOSERVER
        } else {
            core::STREAM_TOCLIENT
        };
        if direction & (core::STREAM_TOSERVER|core::STREAM_TOCLIENT) != dir {
            unsafe { *rdir = dir };
        }
        return 1;
    }
    return 0;
}

#[cfg(test)]
mod tests {

    use crate::dns::dns::DNSState;

    #[test]
    fn test_dns_parse_request_tcp_valid() {
        // A UDP DNS request with the DNS payload starting at byte 42.
        // From pcap: https://github.com/jasonish/suricata-verify/blob/7cc0e1bd0a5249b52e6e87d82d57c0b6aaf75fce/dns-udp-dig-a-www-suricata-ids-org/dig-a-www.suricata-ids.org.pcap
        let buf: &[u8] = &[
            0x00, 0x15, 0x17, 0x0d, 0x06, 0xf7, 0xd8, 0xcb, /* ........ */
            0x8a, 0xed, 0xa1, 0x46, 0x08, 0x00, 0x45, 0x00, /* ...F..E. */
            0x00, 0x4d, 0x23, 0x11, 0x00, 0x00, 0x40, 0x11, /* .M#...@. */
            0x41, 0x64, 0x0a, 0x10, 0x01, 0x0b, 0x0a, 0x10, /* Ad...... */
            0x01, 0x01, 0xa3, 0x4d, 0x00, 0x35, 0x00, 0x39, /* ...M.5.9 */
            0xb2, 0xb3, 0x8d, 0x32, 0x01, 0x20, 0x00, 0x01, /* ...2. .. */
            0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x77, /* .......w */
            0x77, 0x77, 0x0c, 0x73, 0x75, 0x72, 0x69, 0x63, /* ww.suric */
            0x61, 0x74, 0x61, 0x2d, 0x69, 0x64, 0x73, 0x03, /* ata-ids. */
            0x6f, 0x72, 0x67, 0x00, 0x00, 0x01, 0x00, 0x01, /* org..... */
            0x00, 0x00, 0x29, 0x10, 0x00, 0x00, 0x00, 0x00, /* ..)..... */
            0x00, 0x00, 0x00                                /* ... */
        ];

        // The DNS payload starts at offset 42.
        let dns_payload = &buf[42..];

        // Make a TCP DNS request payload.
        let mut request = Vec::new();
        request.push(((dns_payload.len() as u16) >> 8) as u8);
        request.push(((dns_payload.len() as u16) & 0xff) as u8);
        request.extend(dns_payload);

        let mut state = DNSState::new();
        assert_eq!(1, state.parse_request_tcp(&request));
    }

    #[test]
    fn test_dns_parse_request_tcp_short_payload() {
        // A UDP DNS request with the DNS payload starting at byte 42.
        // From pcap: https://github.com/jasonish/suricata-verify/blob/7cc0e1bd0a5249b52e6e87d82d57c0b6aaf75fce/dns-udp-dig-a-www-suricata-ids-org/dig-a-www.suricata-ids.org.pcap
        let buf: &[u8] = &[
            0x00, 0x15, 0x17, 0x0d, 0x06, 0xf7, 0xd8, 0xcb, /* ........ */
            0x8a, 0xed, 0xa1, 0x46, 0x08, 0x00, 0x45, 0x00, /* ...F..E. */
            0x00, 0x4d, 0x23, 0x11, 0x00, 0x00, 0x40, 0x11, /* .M#...@. */
            0x41, 0x64, 0x0a, 0x10, 0x01, 0x0b, 0x0a, 0x10, /* Ad...... */
            0x01, 0x01, 0xa3, 0x4d, 0x00, 0x35, 0x00, 0x39, /* ...M.5.9 */
            0xb2, 0xb3, 0x8d, 0x32, 0x01, 0x20, 0x00, 0x01, /* ...2. .. */
            0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x77, /* .......w */
            0x77, 0x77, 0x0c, 0x73, 0x75, 0x72, 0x69, 0x63, /* ww.suric */
            0x61, 0x74, 0x61, 0x2d, 0x69, 0x64, 0x73, 0x03, /* ata-ids. */
            0x6f, 0x72, 0x67, 0x00, 0x00, 0x01, 0x00, 0x01, /* org..... */
            0x00, 0x00, 0x29, 0x10, 0x00, 0x00, 0x00, 0x00, /* ..)..... */
            0x00, 0x00, 0x00                                /* ... */
        ];

        // The DNS payload starts at offset 42.
        let dns_payload = &buf[42..];

        // Make a TCP DNS request payload but with the length 1 larger
        // than the available data.
        let mut request = Vec::new();
        request.push(((dns_payload.len() as u16) >> 8) as u8);
        request.push(((dns_payload.len() as u16) & 0xff) as u8 + 1);
        request.extend(dns_payload);

        let mut state = DNSState::new();
        assert_eq!(0, state.parse_request_tcp(&request));
    }

    #[test]
    fn test_dns_parse_response_tcp_valid() {
        // A UDP DNS response with the DNS payload starting at byte 42.
        // From pcap: https://github.com/jasonish/suricata-verify/blob/7cc0e1bd0a5249b52e6e87d82d57c0b6aaf75fce/dns-udp-dig-a-www-suricata-ids-org/dig-a-www.suricata-ids.org.pcap
        let buf: &[u8] = &[
            0xd8, 0xcb, 0x8a, 0xed, 0xa1, 0x46, 0x00, 0x15, /* .....F.. */
            0x17, 0x0d, 0x06, 0xf7, 0x08, 0x00, 0x45, 0x00, /* ......E. */
            0x00, 0x80, 0x65, 0x4e, 0x40, 0x00, 0x40, 0x11, /* ..eN@.@. */
            0xbe, 0xf3, 0x0a, 0x10, 0x01, 0x01, 0x0a, 0x10, /* ........ */
            0x01, 0x0b, 0x00, 0x35, 0xa3, 0x4d, 0x00, 0x6c, /* ...5.M.l */
            0x8d, 0x8c, 0x8d, 0x32, 0x81, 0xa0, 0x00, 0x01, /* ...2.... */
            0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x03, 0x77, /* .......w */
            0x77, 0x77, 0x0c, 0x73, 0x75, 0x72, 0x69, 0x63, /* ww.suric */
            0x61, 0x74, 0x61, 0x2d, 0x69, 0x64, 0x73, 0x03, /* ata-ids. */
            0x6f, 0x72, 0x67, 0x00, 0x00, 0x01, 0x00, 0x01, /* org..... */
            0xc0, 0x0c, 0x00, 0x05, 0x00, 0x01, 0x00, 0x00, /* ........ */
            0x0d, 0xd8, 0x00, 0x12, 0x0c, 0x73, 0x75, 0x72, /* .....sur */
            0x69, 0x63, 0x61, 0x74, 0x61, 0x2d, 0x69, 0x64, /* icata-id */
            0x73, 0x03, 0x6f, 0x72, 0x67, 0x00, 0xc0, 0x32, /* s.org..2 */
            0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0xf4, /* ........ */
            0x00, 0x04, 0xc0, 0x00, 0x4e, 0x18, 0xc0, 0x32, /* ....N..2 */
            0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0xf4, /* ........ */
            0x00, 0x04, 0xc0, 0x00, 0x4e, 0x19              /* ....N. */
        ];

        // The DNS payload starts at offset 42.
        let dns_payload = &buf[42..];

        // Make a TCP DNS response payload.
        let mut request = Vec::new();
        request.push(((dns_payload.len() as u16) >> 8) as u8);
        request.push(((dns_payload.len() as u16) & 0xff) as u8);
        request.extend(dns_payload);

        let mut state = DNSState::new();
        assert_eq!(1, state.parse_response_tcp(&request));
    }

    // Test that a TCP DNS payload won't be parsed if there is not
    // enough data.
    #[test]
    fn test_dns_parse_response_tcp_short_payload() {
        // A UDP DNS response with the DNS payload starting at byte 42.
        // From pcap: https://github.com/jasonish/suricata-verify/blob/7cc0e1bd0a5249b52e6e87d82d57c0b6aaf75fce/dns-udp-dig-a-www-suricata-ids-org/dig-a-www.suricata-ids.org.pcap
        let buf: &[u8] = &[
            0xd8, 0xcb, 0x8a, 0xed, 0xa1, 0x46, 0x00, 0x15, /* .....F.. */
            0x17, 0x0d, 0x06, 0xf7, 0x08, 0x00, 0x45, 0x00, /* ......E. */
            0x00, 0x80, 0x65, 0x4e, 0x40, 0x00, 0x40, 0x11, /* ..eN@.@. */
            0xbe, 0xf3, 0x0a, 0x10, 0x01, 0x01, 0x0a, 0x10, /* ........ */
            0x01, 0x0b, 0x00, 0x35, 0xa3, 0x4d, 0x00, 0x6c, /* ...5.M.l */
            0x8d, 0x8c, 0x8d, 0x32, 0x81, 0xa0, 0x00, 0x01, /* ...2.... */
            0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x03, 0x77, /* .......w */
            0x77, 0x77, 0x0c, 0x73, 0x75, 0x72, 0x69, 0x63, /* ww.suric */
            0x61, 0x74, 0x61, 0x2d, 0x69, 0x64, 0x73, 0x03, /* ata-ids. */
            0x6f, 0x72, 0x67, 0x00, 0x00, 0x01, 0x00, 0x01, /* org..... */
            0xc0, 0x0c, 0x00, 0x05, 0x00, 0x01, 0x00, 0x00, /* ........ */
            0x0d, 0xd8, 0x00, 0x12, 0x0c, 0x73, 0x75, 0x72, /* .....sur */
            0x69, 0x63, 0x61, 0x74, 0x61, 0x2d, 0x69, 0x64, /* icata-id */
            0x73, 0x03, 0x6f, 0x72, 0x67, 0x00, 0xc0, 0x32, /* s.org..2 */
            0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0xf4, /* ........ */
            0x00, 0x04, 0xc0, 0x00, 0x4e, 0x18, 0xc0, 0x32, /* ....N..2 */
            0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0xf4, /* ........ */
            0x00, 0x04, 0xc0, 0x00, 0x4e, 0x19              /* ....N. */
        ];

        // The DNS payload starts at offset 42.
        let dns_payload = &buf[42..];

        // Make a TCP DNS response payload, but make the length 1 byte
        // larger than the actual size.
        let mut request = Vec::new();
        request.push(((dns_payload.len() as u16) >> 8) as u8);
        request.push((((dns_payload.len() as u16) & 0xff) + 1) as u8);
        request.extend(dns_payload);

        let mut state = DNSState::new();
        assert_eq!(0, state.parse_response_tcp(&request));
    }
}
