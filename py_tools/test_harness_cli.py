#!/usr/bin/env python3
import argparse
import json
import sys

import zmq


def main():
    parser = argparse.ArgumentParser(description="Test harness CLI (ZMQ REQ/REP)")
    parser.add_argument("--addr", default="tcp://127.0.0.1:5560", help="REP address")
    parser.add_argument("--action", required=True, choices=["ping", "publish", "publish_and_wait"], help="Action")
    parser.add_argument("--event", help="Event name, e.g. EVENT_MARKET_DATA")
    parser.add_argument("--payload", default="{}", help="JSON payload")
    parser.add_argument("--expect", default="[]", help="JSON array of expectations")
    parser.add_argument("--timeout", type=int, default=None, help="Timeout ms")
    parser.add_argument("--req-id", default="", help="Request ID")

    args = parser.parse_args()

    req = {"action": args.action}
    if args.req_id:
        req["req_id"] = args.req_id

    if args.action in ("publish", "publish_and_wait"):
        if not args.event:
            print("--event is required for publish actions", file=sys.stderr)
            return 2
        req["event"] = args.event
        req["payload"] = json.loads(args.payload)

    if args.action == "publish_and_wait":
        req["expect"] = json.loads(args.expect)
        if args.timeout is not None:
            req["timeout_ms"] = args.timeout

    ctx = zmq.Context.instance()
    sock = ctx.socket(zmq.REQ)
    sock.connect(args.addr)
    sock.send_string(json.dumps(req))
    resp = sock.recv_string()
    print(resp)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
