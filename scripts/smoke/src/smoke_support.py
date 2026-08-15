import argparse
import json
import socket
import sys


def check_tcp(endpoints):
    if len(endpoints) % 3 != 0:
        raise SystemExit("check-tcp expects LABEL HOST PORT triples")

    for label, host, port_text in zip(
        endpoints[0::3], endpoints[1::3], endpoints[2::3]
    ):
        port = int(port_text)
        try:
            with socket.create_connection((host, port), timeout=2):
                pass
        except OSError as exc:
            raise SystemExit(f"{label} is not reachable at {host}:{port}: {exc}")


def print_json_fields(fields, file_name):
    if file_name:
        with open(file_name, encoding="utf-8") as source:
            payload = json.load(source)
    else:
        payload = json.load(sys.stdin)
    print(*(payload[field] for field in fields))


def main():
    parser = argparse.ArgumentParser(description="Helpers for WIMI smoke scripts")
    subparsers = parser.add_subparsers(dest="command", required=True)

    tcp_parser = subparsers.add_parser("check-tcp")
    tcp_parser.add_argument("endpoints", nargs="+")

    json_parser = subparsers.add_parser("json-fields")
    json_parser.add_argument("--file")
    json_parser.add_argument("fields", nargs="+")

    args = parser.parse_args()
    if args.command == "check-tcp":
        check_tcp(args.endpoints)
    else:
        print_json_fields(args.fields, args.file)


if __name__ == "__main__":
    main()
