import argparse
import json
import sys

import requests


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    task_parser = subparsers.add_parser("task")
    task_parser.add_argument("instruction")
    task_parser.add_argument("--dod", required=True)
    task_parser.add_argument("--api", default="http://localhost:8000")

    logs_parser = subparsers.add_parser("logs")
    logs_parser.add_argument("task_id")
    logs_parser.add_argument("--api", default="http://localhost:8000")

    patch_parser = subparsers.add_parser("patch")
    patch_parser.add_argument("task_id")
    patch_parser.add_argument("--api", default="http://localhost:8000")

    args = parser.parse_args()

    if args.command == "task":
        response = requests.post(
            f"{args.api}/tasks",
            json={
                "instruction": args.instruction,
                "dod_command": args.dod,
            },
            timeout=10,
        )
        response.raise_for_status()
        print(response.json())
        return 0

    if args.command == "logs":
        response = requests.get(f"{args.api}/tasks/{args.task_id}/artifacts", timeout=10)
        response.raise_for_status()
        print(json.dumps(response.json(), indent=2, ensure_ascii=False))
        return 0

    if args.command == "patch":
        response = requests.get(f"{args.api}/tasks/{args.task_id}/artifacts", timeout=10)
        response.raise_for_status()
        artifacts = response.json()
        patch = next((a for a in artifacts if a["type"] == "patch"), None)
        if not patch:
            print("No patch artifact found", file=sys.stderr)
            return 1
        with open(patch["path"], "r", encoding="utf-8") as handle:
            print(handle.read())
        return 0

    return 1


if __name__ == "__main__":
    raise SystemExit(main())
