"""Read native NetTrace CSVs; distinguish a lost reliable bunch from later buffered RPCs.

Packet sequence pairs are matched across peers, but their local trace clocks are never
subtracted. Business ShotId association is an explicitly labelled order inference;
these CSVs contain property names/bit ranges, not decoded RPC parameter values.
"""

from __future__ import annotations

import argparse
import collections
import csv
import hashlib
import json
import re
from pathlib import Path

RPC_NAME = "ServerSetReplicatedTargetData"
STATUS = {0: "Unknown", 1: "Delivered", 2: "Dropped"}
INT_FIELDS = (
    "GameInstance", "Connection", "PacketSequence", "PacketBytes", "DeliveryStatus",
    "StartBit", "EndBit", "Level", "Channel", "BunchSequence", "Reliable", "ObjectNetId",
)


def read_packets(path: Path) -> list[dict]:
    packets = []
    current = None
    with path.open(encoding="utf-8-sig", newline="") as stream:
        for raw in csv.DictReader(stream):
            row = {key: int(raw[key]) for key in INT_FIELDS}
            row.update(EventName=raw["EventName"], Direction=raw["Direction"], PacketTime=float(raw["PacketTime"]))
            if row["EventName"] == "Packet":
                current = {**row, "Events": []}
                packets.append(current)
            else:
                if current is None or any(row[key] != current[key] for key in
                                          ("GameInstance", "Connection", "Direction", "PacketSequence", "PacketTime")):
                    raise ValueError(f"Content event without its preceding packet in {path}")
                current["Events"].append(row)
    return packets


def bunch_headers(packet: dict) -> list[dict]:
    # The exporter retains level-zero bunch records and selected nested RPC/property
    # records. Nested rows' zero Channel/Reliable fields must not be read as a header.
    return [event for event in packet["Events"] if event["Level"] == 0 and
            (event["Reliable"] or event["Channel"] or event["EventName"] == "Control")]


def requests(packets: list[dict], rpc_name: str = RPC_NAME) -> list[dict]:
    result = []
    for packet in sorted(packets, key=lambda item: item["PacketTime"]):
        for event in packet["Events"]:
            if event["EventName"] != rpc_name:
                continue
            parents = [header for header in bunch_headers(packet) if
                       header["StartBit"] <= event["StartBit"] and header["EndBit"] >= event["EndBit"]]
            if len(parents) != 1:
                raise ValueError(f"RPC has {len(parents)} enclosing bunches in packet {packet['PacketSequence']}")
            result.append({"Packet": packet, "Rpc": event, "Bunch": parents[0]})
    return result


def packet_ref(packet: dict) -> dict:
    return {"PacketSequence": packet["PacketSequence"], "LocalTraceSeconds": packet["PacketTime"],
            "PacketBytes": packet["PacketBytes"], "DeliveryStatus": STATUS.get(packet["DeliveryStatus"], "Invalid")}


def parse_log(path: Path | None, marker: str) -> list[dict]:
    if path is None:
        return []
    result = []
    for index, line in enumerate(path.read_text(encoding="utf-8-sig", errors="replace").splitlines()):
        if marker not in line:
            continue
        fields = dict(re.findall(r"\b([A-Za-z][A-Za-z0-9]*)=([^\s]+)", line))
        fields["LogLine"] = index + 1
        fields["UtcLogPrefix"] = line.partition("]")[0].lstrip("[")
        result.append(fields)
    return result


def analyze(client_packets: list[dict], server_packets: list[dict], client_log: Path | None,
            server_log: Path | None, max_resend_span: float, rpc_name: str = RPC_NAME,
            sender_role: str = "client") -> dict:
    if sender_role not in ("client", "server"):
        raise ValueError("sender_role must be client or server")
    sender_packets, receiver_packets = ((client_packets, server_packets) if sender_role == "client"
                                        else (server_packets, client_packets))
    receiver_role = "server" if sender_role == "client" else "client"
    sender_stream = [p for p in sender_packets if p["Direction"] == "Outgoing"]
    outgoing = requests(sender_stream, rpc_name)
    if not outgoing:
        raise ValueError(f"{sender_role.title()} CSV contains no outgoing {rpc_name} RPC")
    connections = {(r["Packet"]["GameInstance"], r["Packet"]["Connection"]) for r in outgoing}
    if len(connections) != 1:
        raise ValueError("Analyze one sender connection at a time; several transmit the selected RPC")
    sender_game, sender_connection = next(iter(connections))
    sender_stream = [p for p in sender_stream if (p["GameInstance"], p["Connection"]) == (sender_game, sender_connection)]

    # Select the receiving peer connection using packet identities, not ConnectionId,
    # which is process-local and can differ between client and server.
    wanted = {r["Packet"]["PacketSequence"] for r in outgoing}
    candidates = collections.defaultdict(list)
    for packet in receiver_packets:
        if packet["Direction"] == "Incoming":
            candidates[(packet["GameInstance"], packet["Connection"])].append(packet)
    scores = {key: sum(p["PacketSequence"] in wanted for p in value) for key, value in candidates.items()}
    best = max(scores.values(), default=0)
    winners = [key for key, score in scores.items() if score == best]
    if best == 0 or len(winners) != 1:
        raise ValueError("Cannot uniquely match the receiving connection by packet sequence")
    receiver_key = winners[0]
    incoming = candidates[receiver_key]
    received = collections.defaultdict(list)
    for packet in incoming:
        received[packet["PacketSequence"]].append(packet)
    if any(len(value) != 1 for value in received.values()):
        raise ValueError("Repeated packet sequence in this connection; split the trace before sequence wrap")
    received = {key: value[0] for key, value in received.items()}

    client_submits = parse_log(client_log, "CLIENT_SHOT_SUBMIT ")
    sender_business = client_submits if sender_role == "client" else parse_log(server_log, "SHOT_ACK_SEND ")
    # The old protocol emitted one TargetData per shot. New batched/redundant RPCs
    # cannot inherit that correspondence even if two counts happen to be equal.
    order_match = ((sender_role == "client" and rpc_name == RPC_NAME) or
                   (sender_role == "server" and rpc_name == "ClientShotResult")) and len(sender_business) == len(outgoing) and all(
        int(row.get("ShotId", "0")) > 0 for row in sender_business)
    request_by_bunch = {}
    request_rows = []
    for ordinal, request in enumerate(outgoing):
        packet, bunch = request["Packet"], request["Bunch"]
        key = (bunch["Channel"], bunch["BunchSequence"])
        request_by_bunch.setdefault(key, request)
        request_rows.append({"RequestOrdinal": ordinal + 1,
                             "InferredShotId": int(sender_business[ordinal]["ShotId"]) if order_match else None,
                             "Association": "SenderLogOrderOnly_NotPayloadDecoded" if order_match else "Unavailable",
                             "SenderPacket": packet_ref(packet), "Channel": key[0], "BunchSequence": key[1],
                             "Reliable": bool(bunch["Reliable"]), "RpcStartBit": request["Rpc"]["StartBit"],
                             "RpcEndBit": request["Rpc"]["EndBit"],
                             "ReceiverOriginalPacket": packet_ref(received[packet["PacketSequence"]])
                             if packet["PacketSequence"] in received else None})

    groups = collections.defaultdict(list)
    for packet in sender_stream:
        for bunch in bunch_headers(packet):
            if bunch["Reliable"]:
                groups[(bunch["Channel"], bunch["BunchSequence"])].append((packet, bunch))
    retransmissions = []
    for key, occurrences in groups.items():
        occurrences.sort(key=lambda pair: pair[0]["PacketTime"])
        for original, retry in zip(occurrences, occurrences[1:]):
            before, header_before = original
            after, header_after = retry
            interval = after["PacketTime"] - before["PacketTime"]
            if not (0.0 < interval <= max_resend_span) or before["PacketSequence"] == after["PacketSequence"]:
                continue
            same_length = (header_before["EndBit"] - header_before["StartBit"] ==
                           header_after["EndBit"] - header_after["StartBit"])
            if not same_length:
                continue  # Partial flags are absent from the export; do not guess.
            receiver_after = received.get(after["PacketSequence"])
            if receiver_after is None or not any(h["Reliable"] and (h["Channel"], h["BunchSequence"]) == key
                                                 for h in bunch_headers(receiver_after)):
                continue
            later_buffered = []
            for request in outgoing:
                p, h = request["Packet"], request["Bunch"]
                sequence_distance = (h["BunchSequence"] - key[1]) % 4096
                if not h["Reliable"] or h["Channel"] != key[0] or not 0 < sequence_distance < 64:
                    continue
                if not before["PacketTime"] < p["PacketTime"] < after["PacketTime"]:
                    continue
                arrival = received.get(p["PacketSequence"])
                if arrival is None or arrival["DeliveryStatus"] != 1 or arrival["PacketTime"] >= receiver_after["PacketTime"]:
                    continue
                has_bunch = any((e["Channel"], e["BunchSequence"]) == (h["Channel"], h["BunchSequence"])
                                for e in bunch_headers(arrival))
                if not has_bunch:
                    continue
                rpc_count = sum(e["EventName"] == rpc_name for e in arrival["Events"])
                ordinal = outgoing.index(request)
                later_buffered.append({"BunchSequence": h["BunchSequence"],
                                       "InferredShotId": int(sender_business[ordinal]["ShotId"]) if order_match else None,
                                       "ReceiverOriginalPacket": packet_ref(arrival),
                                       "RpcExecutionEventsInOriginalPacket": rpc_count,
                                       "WaitUntilMissingBunchArrivesMs":
                                           (receiver_after["PacketTime"] - arrival["PacketTime"]) * 1000.0})
            retry_rpc_count = sum(e["EventName"] == rpc_name for e in receiver_after["Events"])
            original_request = request_by_bunch.get(key)
            sequence_hol = before["DeliveryStatus"] == 2 and after["DeliveryStatus"] == 1 and bool(later_buffered) and all(
                row["RpcExecutionEventsInOriginalPacket"] == 0 for row in later_buffered) and retry_rpc_count >= len(later_buffered) + int(original_request is not None)
            gap_start = min((row["ReceiverOriginalPacket"]["LocalTraceSeconds"] for row in later_buffered), default=receiver_after["PacketTime"])
            unrelated_unreliable = [packet_ref(p) for p in incoming if gap_start < p["PacketTime"] < receiver_after["PacketTime"]
                                    and any(h["Channel"] == key[0] and not h["Reliable"] for h in bunch_headers(p))]
            original_ordinal = outgoing.index(original_request) if original_request is not None else None
            retransmissions.append({"Channel": key[0], "BunchSequence": key[1],
                                    "InferredShotId": int(sender_business[original_ordinal]["ShotId"])
                                    if order_match and original_ordinal is not None else None,
                                    "SenderOriginal": packet_ref(before), "SenderRetransmission": packet_ref(after),
                                    "SameSenderSendIntervalMs": interval * 1000.0,
                                    "ReceiverRetransmissionPacket": packet_ref(receiver_after),
                                    "RpcExecutionEventsAtRetransmissionArrival": retry_rpc_count,
                                    "LaterReliableRequestsArrivingEarlier": later_buffered,
                                    "SameChannelUnreliablePacketsDuringGap": unrelated_unreliable,
                                    "ReliableHeadOfLineEvidence": sequence_hol})

    client_key = (sender_game, sender_connection) if sender_role == "client" else receiver_key
    server_key = receiver_key if sender_role == "client" else (sender_game, sender_connection)
    return {"SchemaVersion": 2, "RpcName": rpc_name, "SenderRole": sender_role, "ReceiverRole": receiver_role,
            "SenderGameInstance": sender_game, "SenderConnection": sender_connection,
            "ReceiverGameInstance": receiver_key[0], "ReceiverConnection": receiver_key[1],
            "ClientGameInstance": client_key[0], "ClientConnection": client_key[1],
            "ServerGameInstance": server_key[0], "ServerConnection": server_key[1],
            "OutgoingRpcCount": len(outgoing),
            "OutgoingTargetDataCount": len(outgoing) if rpc_name == RPC_NAME else None,
            "BusinessSubmissionCount": len(client_submits), "SenderBusinessEventCount": len(sender_business),
            "OutgoingReliableRpcCount": sum(bool(r["Bunch"]["Reliable"]) for r in outgoing),
            "OutgoingUnreliableRpcCount": sum(not r["Bunch"]["Reliable"] for r in outgoing),
            "SubmissionOrderCorrespondenceAvailable": order_match,
            "ReliableRetransmissions": retransmissions,
            "ReliableHeadOfLineObserved": any(row["ReliableHeadOfLineEvidence"] for row in retransmissions),
            "ServerTimeRejections": parse_log(server_log, "SHOT_TIME_REJECT "),
            "Requests": request_rows,
            "Limits": [
                "ShotId is inferred only from matching submission count/order; RPC parameter values were not decoded.",
                "PacketSequence pairs identify transport packets across peers; process-local Connection ids differ.",
                "Same-channel reliable bunch numbers, repeated transmission and DeliveryStatus establish retransmission.",
                "Reliable head-of-line evidence is scoped to that channel's ordered reliable messages; it is not proof that the entire connection or unreliable movement stopped.",
                "Receive-side replay can annotate buffered RPC execution under the packet that fills a gap; its bit range is not proof that all replayed payloads were originally carried by that packet.",
                "Only same-process trace time differences are used. Cross-process trace origins are not synchronized; no one-way latency is claimed.",
                "Log-order associations are available only for legacy one-shot TargetData or one-shot ClientShotResult; repeated/batched shot submissions are not decoded.",
                "A repeated bunch is accepted only within a bounded interval with equal header bit length; this is not a general decoder for partial bunches or sequence-wrap ambiguity.",
            ]}


def self_test() -> None:
    def packet(seq, time, status, bunch=None, rpc=False):
        p = dict(GameInstance=0, Connection=1, Direction="Outgoing", PacketSequence=seq,
                 PacketTime=time, PacketBytes=80, DeliveryStatus=status, Events=[])
        if bunch is not None:
            p["Events"].append(dict(EventName="Actor", StartBit=80, EndBit=600, Level=0,
                                    Channel=11, BunchSequence=bunch, Reliable=1))
        if rpc:
            p["Events"].append(dict(EventName=RPC_NAME, StartBit=160, EndBit=580, Level=2,
                                    Channel=0, BunchSequence=0, Reliable=0))
        return p

    client = [packet(10, 1.0, 2, 4, True), packet(11, 1.1, 1, 5, True), packet(12, 1.3, 1, 4)]
    server = [packet(10, 9.0, 2), packet(11, 9.1, 1, 5), packet(12, 9.3, 1, 4, True)]
    server[-1]["Events"].append(dict(server[-1]["Events"][-1]))
    for p in server:
        p.update(Connection=2, Direction="Incoming")
    result = analyze(client, server, None, None, 2.0)
    assert result["ReliableHeadOfLineObserved"]
    assert abs(result["ReliableRetransmissions"][0]["SameSenderSendIntervalMs"] - 300) < .001
    assert abs(result["ReliableRetransmissions"][0]["LaterReliableRequestsArrivingEarlier"][0]["WaitUntilMissingBunchArrivesMs"] - 200) < .001
    assert all(r["InferredShotId"] is None for r in result["Requests"])
    # Downlink uses the real server/client positions; do not swap filenames and then
    # mislabel the server's dropped original as a client send.
    downlink = analyze(server, client, None, None, 2.0, sender_role="server")
    assert downlink["SenderRole"] == "server" and downlink["ReceiverRole"] == "client"
    assert downlink["ServerConnection"] == 1 and downlink["ClientConnection"] == 2
    assert downlink["ReliableHeadOfLineObserved"]
    assert downlink["ReliableRetransmissions"][0]["SenderOriginal"]["PacketSequence"] == 10
    unreliable_client = [packet(50, 3.0, 1, 0, True)]
    unreliable_client[0]["Events"][0]["Reliable"] = 0
    unreliable_server = [packet(50, 40.0, 1, 0, True)]
    unreliable_server[0].update(Direction="Incoming", Connection=8)
    unreliable_server[0]["Events"][0]["Reliable"] = 0
    unreliable = analyze(unreliable_client, unreliable_server, None, None, 2.0)
    assert unreliable["OutgoingUnreliableRpcCount"] == 1
    assert unreliable["OutgoingReliableRpcCount"] == 0
    assert not unreliable["ReliableHeadOfLineObserved"]
    # A burst alone and a different channel must not be reported as this reliable gap.
    clean = analyze(client[:2], server, None, None, 2.0)
    assert not clean["ReliableHeadOfLineObserved"]
    server[1]["Events"][0]["Channel"] = 12
    assert not analyze(client, server, None, None, 2.0)["ReliableHeadOfLineObserved"]
    print("AnalyzeFpsNetworkTrace self-test passed (uplink/downlink roles, unreliable parent, retransmission, HOL, clock separation, negative controls)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client-csv", type=Path)
    parser.add_argument("--server-csv", type=Path)
    parser.add_argument("--client-log", type=Path)
    parser.add_argument("--server-log", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--max-resend-span", type=float, default=2.0)
    parser.add_argument("--rpc-name", default=RPC_NAME)
    parser.add_argument("--sender-role", choices=("client", "server"), default="client")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if args.client_csv is None or args.server_csv is None or args.output is None or args.max_resend_span <= 0:
        parser.error("--client-csv, --server-csv, --output and positive --max-resend-span are required")
    if args.output.exists():
        parser.error("Output already exists; choose a new evidence path")
    result = analyze(read_packets(args.client_csv), read_packets(args.server_csv), args.client_log, args.server_log,
                     args.max_resend_span, args.rpc_name, args.sender_role)
    result["Inputs"] = [{"Path": str(path.resolve()), "Sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                        for path in (args.client_csv, args.server_csv, args.client_log, args.server_log) if path is not None]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps({"Output": str(args.output.resolve()), "RpcName": result["RpcName"], "SenderRole": result["SenderRole"], "OutgoingRpcCount": result["OutgoingRpcCount"],
                      "ReliableRetransmissions": len(result["ReliableRetransmissions"]),
                      "ReliableHeadOfLineObserved": result["ReliableHeadOfLineObserved"]}))


if __name__ == "__main__":
    main()
