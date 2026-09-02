# QMI data plane from bare metal — what is proven, and what is not

The MCU drives the modem's data path itself: no PC, no vendor driver, no
`AT+QCRMTOOL`. QMI control messages ride EP0 as CDC encapsulated commands;
the QMI interface's bulk pair carries packets.

Target: Quectel RM500Q-GL, `2c7c:0800`, firmware `RM500QGLABR11A04M4G`,
on a commercial LTE network. Host: STM32H745 + `src/usbh.c`, full speed.

## The composition this drives

```
IF0  ff/ff/30  DIAG   bulk 81/01
IF1  ff/00/00  NMEA   int 83 + bulk 82/02      <- also the voice PCM channel
IF2  ff/00/00  AT     int 85 + bulk 84/03
IF3  ff/00/00  Modem  int 87 + bulk 86/04
IF4  ff/ff/ff  QMI    int 88 + bulk 8e/0f      <- control on EP0, data here
```

## Transport

```
send     bmRequestType 0x21  bRequest 0x00  SEND_ENCAPSULATED_COMMAND
receive  bmRequestType 0xA1  bRequest 0x01  GET_ENCAPSULATED_RESPONSE
wIndex   the QMI interface number (4)
```

DTR must be asserted on IF4 first (`SET_CONTROL_LINE_STATE`) — the kernel
marks this VID:PID with `QMI_WWAN_QUIRK_DTR` for the same reason.

The interrupt endpoint would signal `RESPONSE_AVAILABLE`; polling
`GET_ENCAPSULATED_RESPONSE` works without it. While the module has nothing
to say it answers with a zero-length data stage, which is what makes polling
safe.

## Proven on hardware

| step | result |
|---|---|
| find IF4, assert DTR | `qmi: interface 4 (ctrl EP0, data 8e/0f, int 88)` |
| CTL 0x0022 ALLOCATE_CID (WDA) | client id 1 |
| CTL 0x0022 ALLOCATE_CID (WDS) | client id 15 |
| WDA 0x0020 SET_DATA_FORMAT | OK, raw IP |
| WDS 0x00A2 BIND_MUX_DATA_PORT | OK, iface 4 |
| WDS 0x0020 START_NETWORK `<apn>` | packet handle returned |
| WDS 0x002D GET_CURRENT_SETTINGS | **10.x.y.176 / 255.255.255.224, gw .177, MTU 1500** |
| bulk OUT of a raw IP packet | accepted, forwarded onto the air |
| bulk IN | delivered the network's reply |

The address the MCU obtained is the same one the PC got from the same SIM.
**The MCU holds a real cellular IP with no host driver anywhere in the loop.**

## The GET_CURRENT_SETTINGS TLVs this firmware actually sends

Walked live off the wire, not guessed:

| TLV | len | meaning | value seen |
|---|---|---|---|
| `0x02` | 4 | result | success |
| `0x10` | 8 | profile name | `profile1` |
| `0x11` | 1 | PDP type | 0 = IPv4 |
| `0x14` | n | APN | `<apn>` |
| `0x1E` | 4 | **IPv4 address** | `b0 yy xx 0a` → 10.x.y.176 |
| `0x20` | 4 | **IPv4 gateway** | 10.x.y.177 |
| `0x21` | 4 | **subnet mask** | 255.255.255.224 |
| `0x29` | 4 | **MTU** | 1500 |
| `0x2B` | 1 | IP family | 4 |

Addresses are u32 **little endian** on the wire: `b0 yy xx 0a` is
10.x.y.176, not 176.y.x.10. (Carrier addresses in this document are redacted
to `x`/`y`; the byte order is what matters.) Getting this backwards is the classic
QMI trap.

**There is no DNS TLV in this response.** `dns 0.0.0.0` is honest reporting,
not a parse bug — the operator did not supply one over QMI for this bearer.

## Three real bugs this found

**1. `GET_DATA_FORMAT`'s endpoint-info TLV is `0x11`, not `0x17`.**
`0x17` is the id in the *Set* message, where it has to be high because
`0x11`–`0x16` are taken by link protocol, UL/DL aggregation, NDP signature
and the DL limits. The *Get* message has only `0x10` (QoS) and `0x11`
(endpoint id). Sending `0x17` there earns **error 17 MISSING_ARG**: the
module ignores the unknown TLV, then faults because `ep_id` was never
supplied. The bench log matches this exactly.

**2. Client IDs leak, and the leak is module-side.**
`CTL ALLOCATE_CID` hands out state that survives a *host* reset. Flashing
and running the app repeatedly leaked two ids per run, and after a handful
of runs the module's CTL service stopped answering `ALLOCATE_CID` at all —
no error code, no response, just silence. `CTL 0x0023 RELEASE_CID` on every
exit path is the fix; a modem reboot is the only recovery once it happens.

**3. Responses were matched on message id + transaction id alone.**
Message ids are not unique across services: `CTL ALLOCATE_CID` and
`WDA GET_DATA_FORMAT` are **both `0x0022`**, and this driver issues both. The
receive path also derived the header size (6 vs 7 bytes) from the service it
was *waiting on* rather than the one that *arrived*, so a CTL frame read as a
service frame puts every field one byte out. A frame is now accepted only
when direction (`0x80`), service, client id, the response-vs-indication flag,
message id and transaction id all agree, and the mandatory result TLV is
present — its absence used to read as success. CTL and the services also now
keep separate transaction counters, because CTL's field is one byte wide and
rewinding a shared counter to fit it re-used a service's id.

Verified against the real frame: `01 13 00 80 1a 01 02 09 00 22 00 07 00 …`
— `80` device→host, `1a` WDA, `01` client, `02` response.
`tests/host/test_m4.c` has a decoy carrying our own msg+txn under the *wrong*
service; it fails against the old matcher and passes against the new one.

## PROVEN: a full IP round trip over LTE, from bare metal

With raw-IP framing and **no** mux bind, an ICMP echo to the operator's own
router completes end to end:

```
tx  45 00 00 24 00 00 40 00 40 01 .. ..  0a xx yy b0  0a .. .. ..
    08 00 31 73  48 37  00 01  68 37 34 35 72 74 6f 73
        type 08 = echo request        id 4837  seq 0001  "h745rtos"

rx  45 38 00 24 00 00 40 00 f9 01 .. ..  0a .. .. ..  0a xx yy b0
    00 00 39 73  48 37  00 01  68 37 34 35 72 74 6f 73
        type 00 = echo REPLY          id 4837  seq 0001  "h745rtos"
```

Same identifier, same sequence number, our own payload returned intact by a
host inside the operator's network. The chain that carries it:

> from-scratch RTOS → bare-metal DWC2 USB host → QMI over EP0 → LTE bearer →
> operator router → and all the way back

`data: operator router -> REPLY (bearer carries traffic)`

**The mux bind is not called on the working path** -- but the reason is NOT
the one this document previously gave. It claimed binding "stopped packets
moving entirely" and that the message was "confirmed correct". Both were
wrong: `qmi_bind_mux()` wrote the mux id at request offset 15, which is the
TYPE byte of the following client-type TLV rather than the value of the mux
TLV at offset 14. Every bind therefore sent mux id `0x00` regardless of what
was asked for, destroyed the client-type TLV, and injected a garbage one --
and the module accepted it without complaint. So the two "binding breaks
traffic" bench runs measured a malformed message and prove nothing about
binding. The offset is fixed and pinned by a test; whether a *valid* bind
helps or hurts on this module is simply untested.

## RESOLVED: the stack reaches the public internet

A second SIM settled everything below. Same modem, same firmware, same driver —
only the card changed.

```
[3b] public internet
    ping 8.8.8.8 ................ REPLY
    *** example.com = 172.66.147.243 ***  61-byte answer over LTE

[4] verdict
    QMI control over EP0 .............. PASS
    bearer + IP configuration ......... PASS
    bidirectional IP over LTE ......... PASS (18/20, 39/52/61 ms)
    full-size packets, verified ....... PASS (6/6, up to 1400 B)
    public internet ................... PASS
    name resolution ................... PASS
```

Second operator, LTE band 7, a `/29` carrier address with a gateway, an
operator DNS server, MTU 1500.

**The diagnosis in the sections below was right, and the contrast proves it:**

```
SIM A:  +CGCONTRDP: 1,5,<apn>,<addr>,,,
SIM B:  +CGCONTRDP: 1,5,<apn>,<addr>,<v6>,fe80::1,<dns1>,<dns2>
                            address       gateway  DNS 1  DNS 2
```

Address-only versus fully provisioned. QMI also returns a real DNS address here
where it returned `0.0.0.0` on the other SIM — that parser was always correct.

**One driver defect did surface, and it was ours:** the flow went from "the AT
port answers" straight to `START_NETWORK`, but an answering AT port is not a
ready modem. The SIM finished initialising 2.3 s after AT began responding, and
registration took 23.3 s. `wait_for_service()` now waits for `+CPIN: READY` and
for `+CEREG?`/`+CREG?` to report registered before any data call. Parse the
`<stat>` field positionally: `+CEREG: 1,0` contains `",0"` and a substring
search reads "not registered" as registered.

Everything below remains accurate for the first SIM (SIM A) and is kept as the record of
how the provisioning fault was found.

## The APN is right; the route is missing (and that is the network's doing)

Uplink reaches the operator. A raw-IP ICMP echo to 8.8.8.8 draws a reply —
but the reply is `ICMP type 3 code 1`, destination unreachable / host
unreachable, from an operator router four hops in, quoting our datagram back:

```
tx  45 00 00 24 00 00 40 00 40 01 .. .. 0a xx yy b0 08 08 08 08 08 00 ...
rx  45 00 00 38 .. .. 00 00 f9 01 .. .. 0a .. .. .. 0a xx yy b0 03 01 ...
                                          ^^ operator router ^^ type 3 code 1
```

The same answer comes back for UDP/53 to 8.8.8.8 and to 1.1.1.1, and the
bearer's own gateway never replies. So this is not an ICMP policy filter —
it is "no route" for everything.

What that rules out, and what it does not:

- **Framing is settled.** Raw IP got answers off the network; QMAP framing
  with mux `0x81` got nothing at all. The pipe carries bare IP.
- **The USB data path works in both directions.** Our packet went out and
  the network's reply came back on the bulk IN pipe.
- **The bearer carries traffic.** The operator router answers our pings; 8.8.8.8 and
  1.1.1.1 do not, and the refusal comes from that same router. So the driver,
  the USB path, the bearer and the IP stack are all working, and what is left
  is a routing/APN/subscription question rather than a defect in this code.
- The `same packet handle across resets` theory was wrong: after a module
  reboot `START_NETWORK` returned a different handle, so it does
  create a call rather than reusing one.

### ROOT CAUSE: the network provisions this PDN without a gateway

`AT+CGCONTRDP` asked the module what the NETWORK actually assigned to each
context. Per 3GPP TS 27.007 the fields are `cid, bearer_id, apn, local_addr,
gw_addr, DNS_prim, DNS_sec, P-CSCF_prim, P-CSCF_sec`:

```
+CGCONTRDP: 1,5,<apn>,<addr>,,,
                     address ^ no gateway, no DNS

+CGCONTRDP: 2,6,ims,<v6 addr>,fe80::1,,,<pcscf1>,<pcscf2>
                    address   gateway   P-CSCF primary, secondary
```

The IMS PDN is fully provisioned -- address, gateway, and both P-CSCF servers,
which is exactly why voice calls work on this SIM. The internet PDN gets an
address and **nothing else**: no gateway, no DNS.

So the `.177` "gateway" was never assigned by the network. Both this driver and
Windows synthesised it as "address + 1", the usual point-to-point convention --
which is why it never answers. There is nothing to route through because the
operator did not provide a route.

`AT+CEER` reports `Regular deactivation` (no fault).

**This is a provisioning matter on the subscription, not a defect anywhere in
this code.** Concretely, for the operator: context 1 activates and
receives an IP, but the PCO comes back with no gateway and no DNS, while the IMS
context is provisioned in full.

### Corroboration: the vendor driver fails identically

The modem was moved to the PC and Quectel's own certified Windows driver was
allowed to bring up the same APN. It derives **exactly** the same
configuration this MCU driver derives from QMI:

```
InterfaceDescription : Quectel Wireless Ethernet Adapter
IPv4Address          : 10.x.y.176          <- identical
IPv4DefaultGateway   : 10.x.y.177          <- identical
DNSServer            : fec0:0:0:ffff::1..3 <- Windows' "none supplied" placeholder
```

And it gets exactly the same answers from the network:

| destination (source 10.x.y.176) | PC, Quectel driver | H745, this driver |
|---|---|---|
| operator router | **Reply, TTL 249** | **Reply, TTL 249** |
| 8.8.8.8 | `Destination host unreachable` from the operator router | same, same source |
| 10.x.y.177 bearer gateway | timeout | timeout |
| TCP 1.1.1.1:443, 8.8.8.8:53, 93.184.216.34:80 | no connect | — |
| UDP/53 to 8.8.8.8 | no answer | no answer |

Control: the same PC reaches `1.1.1.1:443` and resolves `example.com` normally
over its other interface, so the machine's connectivity is fine.

**The vendor driver and this one are functionally equivalent on this network.**
Everything a host can do has been done; the operator is refusing to route this
bearer off its own network. That is a subscription/provisioning matter, and no
change to this code can fix it.

### If the subscription is later fixed

Re-run the bench app: it needs no changes. The one experiment still genuinely
open is whether a *valid* `BIND_MUX_DATA_PORT` (now that the byte layout is
correct) changes anything -- expected to be a no-op, but it has never actually
been sent.

## Host-side simulation

`tests/host/test_m4.c` runs the real `qmi.c` through the real `usbh.c`
against an emulated Qualcomm modem added to `sim_hw.c`. 53 checks covering
the CTL vs service header split, the little-endian address decode,
indications interleaved ahead of replies, slow replies, QMI error results,
an oversize request being refused rather than truncated onto the wire, and
the cross-service message-id alias. Total host suite: 224 checks.

The sim proves the encoding. It cannot prove the firmware agrees — that is
what the bench is for, and the two bugs above are exactly the kind the sim
could not have caught.
