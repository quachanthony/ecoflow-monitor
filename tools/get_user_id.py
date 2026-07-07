#!/usr/bin/env python3
"""Fetch your numeric EcoFlow user id for src/secrets.h.

The device only accepts BLE clients that present MD5(user_id + serial), so the
firmware needs the same user id the EcoFlow app used when you paired the unit.

Usage:
    python3 tools/get_user_id.py you@example.com [--region api|api-e|api-a|api-j|api-r|api-cn]

Ported from ha-ef-ble custom_components/ef_ble/eflib/login.py (stdlib only).
"""

import argparse
import base64
import getpass
import json
import sys
import urllib.request

REGIONS = ["api", "api-e", "api-a", "api-j", "api-r", "api-cn"]


def login(base_url: str, identifier: str, password: str, is_phone: bool) -> str:
    payload = {
        "scene": "IOT_APP",
        "appVersion": "1.0.0",
        "password": base64.b64encode(password.encode()).decode(),
        "oauth": {"bundleId": "com.ef.EcoFlow"},
        "userType": "ECOFLOW",
    }
    if is_phone:
        payload["phone"] = identifier.removeprefix("+86")
    else:
        payload["email"] = identifier

    req = urllib.request.Request(
        f"https://{base_url}/auth/login",
        data=json.dumps(payload).encode(),
        headers={"Accept": "application/json", "Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        result = json.load(resp)

    if result.get("code") != "0":
        raise SystemExit(f"Login failed: {result.get('message')}")
    return result["data"]["user"]["userId"]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("identifier", help="EcoFlow account email (or +86 phone for api-cn)")
    parser.add_argument("--region", choices=REGIONS, default=None,
                        help="API host (default: auto)")
    args = parser.parse_args()

    digits = args.identifier.removeprefix("+").replace(" ", "")
    is_phone = digits.isdigit() and 6 <= len(digits) <= 15

    region = args.region or ("api-cn" if is_phone else "api")
    if region == "api-cn" and not is_phone:
        raise SystemExit("api-cn requires a phone number, not an email")

    password = getpass.getpass("EcoFlow password: ")
    user_id = login(f"{region}.ecoflow.com", args.identifier, password,
                    is_phone and region == "api-cn")

    print(f"\nYour user id: {user_id}")
    print('Put it in src/secrets.h as:  #define ECOFLOW_USER_ID "' + user_id + '"')


if __name__ == "__main__":
    sys.exit(main())
