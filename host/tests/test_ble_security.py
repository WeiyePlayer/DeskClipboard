import struct
import unittest
import uuid

from clipboard_agent.ble_security import Cipher, Handshake, Stream, derive
from clipboard_agent.hub_protocol import HubError


class BLESecurityTest(unittest.TestCase):
    def setUp(self):
        self.board = "deskclip-84c7bb78dc60"
        self.device = str(uuid.uuid4())
        self.token = "ab" * 32

    def test_handshake_authenticates_board_and_never_sends_secret(self):
        client = Handshake(self.board, self.device, self.token)
        request = client.request()
        self.assertEqual(len(request), 53)
        self.assertNotIn(bytes.fromhex(self.token), request)
        server_nonce = bytes(range(16))
        proof, key = derive(self.token, self.board, self.device, client.nonce, server_nonce)
        cipher = client.finish(b"H" + server_nonce + proof)
        server = Cipher(key, client=False)
        self.assertEqual(server.decrypt(cipher.encrypt("中文🙂".encode())), "中文🙂".encode())
        self.assertEqual(cipher.decrypt(server.encrypt(b"original PNG")), b"original PNG")

    def test_wrong_board_and_wrong_secret_cannot_impersonate_device(self):
        for board, token in (("other-board", self.token), (self.board, "cd" * 32)):
            client = Handshake(self.board, self.device, self.token)
            proof, _ = derive(token, board, self.device, client.nonce, b"a" * 16)
            with self.assertRaisesRegex(HubError, "authentication_failed"):
                client.finish(b"H" + b"a" * 16 + proof)

    def test_tampering_replay_and_reflection_are_rejected(self):
        for attack in ("tamper", "replay", "reflect"):
            client, server = Cipher(b"a" * 32), Cipher(b"a" * 32, client=False)
            record = client.encrypt(b"private")
            if attack == "tamper":
                record = record[:-1] + bytes([record[-1] ^ 1])
            elif attack == "replay":
                server.decrypt(record)
            target = client if attack == "reflect" else server
            with self.assertRaises(HubError):
                target.decrypt(record)

    def test_new_session_rejects_old_ciphertext(self):
        _, a = derive(self.token, self.board, self.device, b"a" * 16, b"b" * 16)
        _, b = derive(self.token, self.board, self.device, b"c" * 16, b"d" * 16)
        with self.assertRaises(HubError):
            Cipher(b, client=False).decrypt(Cipher(a).encrypt(b"old job"))

    def test_fragmented_and_coalesced_att_records(self):
        stream = Stream()
        records = [b"x", b"y" * 16000, b"z" * 300]
        framed = b"".join(struct.pack("<I", len(r)) + r for r in records)
        found = []
        for offset in range(0, len(framed), 17):
            found.extend(stream.feed(framed[offset:offset + 17]))
        self.assertEqual(found, records)
        for size in (0, 1024 * 1024 + 1):
            with self.assertRaises(HubError):
                Stream().feed(struct.pack("<I", size))


if __name__ == "__main__": unittest.main()
