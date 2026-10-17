"""Write android/app/src/test/resources/deskclip_vectors.json: protocol test
vectors from the Python implementation (the reference the firmware is tested
against) for the Kotlin unit tests. Deterministic; rerun after protocol changes.

  cd host && ../.venv/bin/python gen_android_vectors.py
"""
import json
import struct
from pathlib import Path

from clipboard_agent import ble_security, pake
from clipboard_agent.extract import extract_chips
from clipboard_agent.frames import Region, encode_region, REGION_BODY, REGION_MENU, REGION_CHIP
from tests.test_pake import RFC

OUT = Path(__file__).resolve().parents[1] / "android/app/src/test/resources/deskclip_vectors.json"


def pairing_vector(code, x, y, device, board, name):
    client = pake.PairClient(code, device, name, "android", x=x)
    server = pake.PairServer(code, board, y=y)
    p1 = client.start()
    p2 = server.reply(p1)
    p3 = client.confirm(p2)
    p4 = server.finish(p3)
    assert client.finish(p4) == server.token
    return dict(code=code, x=format(x, "064x"), y=format(y, "064x"), device_id=device, board_id=board,
                name=name, w=format(client.w, "064x"), p1=p1.hex(), p2=p2.hex(), p3=p3.hex(), p4=p4.hex(),
                token=server.token)


def main():
    vectors = {"spake2_rfc9382": [{k: (v.decode() if isinstance(v, bytes) else v) for k, v in r.items()} for r in RFC]}
    vectors["pairing"] = [
        pairing_vector("123456", 0x1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef,
                       0x0fedcba987654321fedcba987654321fedcba987654321fedcba987654321fed,
                       "0f6d1c2e-6a3b-4c5d-8e9f-a0b1c2d3e4f5", "deskclip-84c7bb78dc60", "Pixel 8"),
        pairing_vector("000042", 7, 11, "7e57a11d-0000-4000-8000-000000000001", "deskclip-000000000001", "手机"),
    ]
    vectors["pairing_error"] = (b"P\xffbad_code").hex()

    token = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
    board, device = "deskclip-84c7bb78dc60", "0f6d1c2e-6a3b-4c5d-8e9f-a0b1c2d3e4f5"
    client_nonce, server_nonce = bytes(range(16)), bytes(range(100, 116))
    proof, session = ble_security.derive(token, board, device, client_nonce, server_nonce)
    vectors["handshake"] = dict(token=token, board_id=board, device_id=device, client_nonce=client_nonce.hex(),
                                server_nonce=server_nonce.hex(), proof=proof.hex(), session=session.hex(),
                                request=(b"H" + device.encode() + client_nonce).hex(),
                                reply=(b"H" + server_nonce + proof).hex())
    sender = ble_security.Cipher(session, client=True)
    board_side = ble_security.Cipher(session, client=False)
    plains = [b'J{"op":"status"}', "中文 ✓".encode(), b"B" + bytes(300)]
    vectors["cipher"] = dict(key=session.hex(),
                             client_records=[dict(plain=p.hex(), record=sender.encrypt(p).hex()) for p in plains],
                             board_records=[dict(plain=p.hex(), record=board_side.encrypt(p).hex()) for p in plains])
    record = bytes(i % 251 for i in range(600))
    framed = struct.pack("<I", len(record)) + record
    vectors["stream"] = dict(record=record.hex(), framed=framed.hex(), fragment=244,
                             fragments=[framed[i:i + 244].hex() for i in range(0, len(framed), 244)])
    texts = ["没有特殊内容", "验证码：123456，请在 5 分钟内使用", "电话 13812345678 或 a.b@example.com",
             "https://example.com/a?b=1 和 http://x.cn/path", "a@b.co c@d.io e@f.cc g@h.dd",
             "号码13812345678x 13912345678"]
    vectors["chips"] = [dict(text=t, chips=extract_chips(t)) for t in texts]
    regions = [Region(REGION_BODY, 0, 0, 0, 180, 70), Region(REGION_MENU, 0, 180, 0, 44, 70),
               Region(REGION_CHIP, 0, 10, 56, 120, 24)]
    vectors["card_header"] = dict(width=224, height=70, regions=[[r.kind, r.chip_index, r.x, r.y, r.w, r.h] for r in regions],
                                  bytes=(struct.pack("<IBHHB", 0, 0, 224, 70, len(regions))
                                         + b"".join(encode_region(r) for r in regions)).hex())
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(vectors, ensure_ascii=False, indent=1) + "\n")
    print(OUT)


if __name__ == "__main__":
    main()
