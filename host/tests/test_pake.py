import unittest

from clipboard_agent import pake

# RFC 9382 Appendix B (SPAKE2-P256-SHA256-HKDF-HMAC).
RFC = [
    dict(A=b"server", B=b"client",
         w="2ee57912099d31560b3a44b1184b9b4866e904c49d12ac5042c97dca461b1a5f",
         x="43dd0fd7215bdcb482879fca3220c6a968e66d70b1356cac18bb26c84a78d729",
         pA="04a56fa807caaa53a4d28dbb9853b9815c61a411118a6fe516a8798434751470f9010153ac33d0d5f2047ffdb1a3e42c9b4e6be662766e1eeb4116988ede5f912c",
         y="dcb60106f276b02606d8ef0a328c02e4b629f84f89786af5befb0bc75b6e66be",
         pB="0406557e482bd03097ad0cbaa5df82115460d951e3451962f1eaf4367a420676d09857ccbc522686c83d1852abfa8ed6e4a1155cf8f1543ceca528afb591a1e0b7",
         K="0412af7e89717850671913e6b469ace67bd90a4df8ce45c2af19010175e37eed69f75897996d539356e2fa6a406d528501f907e04d97515fbe83db277b715d3325",
         Ke="0e0672dc86f8e45565d338b0540abe69", KcA="00c12546835755c86d8c0db7851ae86f",
         KcB="a9fa3406c3b781b93d804485430ca27a",
         cA="58ad4aa88e0b60d5061eb6b5dd93e80d9c4f00d127c65b3b35b1b5281fee38f0",
         cB="d3e2e547f1ae04f2dbdbf0fc4b79f8ecff2dff314b5d32fe9fcef2fb26dc459b"),
    dict(A=b"", B=b"client",
         w="0548d8729f730589e579b0475a582c1608138ddf7054b73b5381c7e883e2efae",
         x="403abbe3b1b4b9ba17e3032849759d723939a27a27b9d921c500edde18ed654b",
         pA="04a897b769e681c62ac1c2357319a3d363f610839c4477720d24cbe32f5fd85f44fb92ba966578c1b712be6962498834078262caa5b441ecfa9d4a9485720e918a",
         y="903023b6598908936ea7c929bd761af6039577a9c3f9581064187c3049d87065",
         pB="04e0f816fd1c35e22065d5556215c097e799390d16661c386e0ecc84593974a61b881a8c82327687d0501862970c64565560cb5671f696048050ca66ca5f8cc7fc",
         K="048f83ec9f6e4f87cc6f9dc740bdc2769725f923364f01c84148c049a39a735ebda82eac03e00112fd6a5710682767cff5361f7e819e53d8d3c3a2922e0d837aa6",
         Ke="642f05c473c2cd79909f9a841e2f30a7", KcA="c6be376fc7cd1301fd0a13adf3e7bffd",
         KcB="b7243f4ae60440a49b3f8cab3c1fba07",
         cA="47d29e6666af1b7dd450d571233085d7a9866e4d49d2645e2df975489521232b",
         cB="3313c5cefc361d27fb16847a91c2a73b766ffa90a4839122a9b70a2f6bd1d6df"),
    dict(A=b"", B=b"",
         w="7bf46c454b4c1b25799527d896508afd5fc62ef4ec59db1efb49113063d70cca",
         x="8cef65df64bb2d0f83540c53632de911b5b24b3eab6cc74a97609fd659e95473",
         pA="04a65b367a3f613cf9f0654b1b28a1e3a8a40387956c8ba6063e8658563890f46ca1ef6a676598889fc28de2950ab8120b79a5ef1ea4c9f44bc98f585634b46d66",
         y="d7a66f64074a84652d8d623a92e20c9675c61cb5b4f6a0063e4648a2fdc02d53",
         pB="04589f13218822710d98d8b2123a079041052d9941b9cf88c6617ddb2fcc0494662eea8ba6b64692dc318250030c6af045cb738bc81ba35b043c3dcb46adf6f58d",
         K="041a3c03d51b452537ca2a1fea6110353c6d5ed483c4f0f86f4492ca3f378d40a994b4477f93c64d928edbbcd3e85a7c709b7ea73ee97986ce3d1438e135543772",
         Ke="fc6374762ba5cf11f4b2caa08b2cd1b9", KcA="5dbd2f477166b7fb6d61febbd77a5563",
         KcB="7689b4654407a5faeffdc8f18359d8a3",
         cA="dfb4db8d48ae5a675963ea5e6c19d98d4ea028d8e898dad96ea19a80ade95dca",
         cB="d0f0609d1613138d354f7e95f19fb556bf52d751947241e8c7118df5ef0ae175"),
]


class Spake2Vectors(unittest.TestCase):
    def test_rfc9382_appendix_b(self):
        for v in RFC:
            w, x, y = (int(v[k], 16) for k in ("w", "x", "y"))
            pa = pake.encode(pake.add(pake.mul(x, pake.G), pake.mul(w, pake.M)))
            pb = pake.encode(pake.add(pake.mul(y, pake.G), pake.mul(w, pake.N)))
            self.assertEqual(pa.hex(), v["pA"])
            self.assertEqual(pb.hex(), v["pB"])
            ka = pake.mul(x, pake.add(pake.decode(pb), pake.neg(pake.mul(w, pake.N))))
            kb = pake.mul(y, pake.add(pake.decode(pa), pake.neg(pake.mul(w, pake.M))))
            self.assertEqual(pake.encode(ka).hex(), v["K"])
            self.assertEqual(ka, kb)
            tt = pake.transcript(v["A"], v["B"], pa, pb, pake.encode(ka), w)
            ke, kca, kcb, ca, cb = pake.schedule(tt)
            self.assertEqual((ke.hex(), kca.hex(), kcb.hex()), (v["Ke"], v["KcA"], v["KcB"]))
            self.assertEqual(cb.hex(), v["cB"])
            self.assertEqual(ca.hex(), v["cA"])

    def test_points_and_validation(self):
        self.assertTrue(pake.on_curve(pake.M) and pake.on_curve(pake.N))
        with self.assertRaises(ValueError):
            pake.decode(b"\x04" + b"\x01" * 64)


class Pairing(unittest.TestCase):
    ID = "0f6d1c2e-6a3b-4c5d-8e9f-a0b1c2d3e4f5"

    def run_pair(self, client_code, board_code):
        client = pake.PairClient(client_code, self.ID, "Pixel 8", "android")
        board = pake.PairServer(board_code, "deskclip-84c7bb78dc60")
        confirm = client.confirm(board.reply(client.start()))
        return client, board, board.finish(confirm)

    def test_matching_code_derives_same_credential(self):
        client, board, done = self.run_pair("123456", "123456")
        token = client.finish(done)
        self.assertEqual(token, board.token)
        self.assertEqual(len(bytes.fromhex(token)), 32)
        self.assertEqual(client.board_id, "deskclip-84c7bb78dc60")
        self.assertEqual((board.name, board.platform), ("Pixel 8", "android"))

    def test_wrong_code_fails_on_both_sides(self):
        client, board, done = self.run_pair("123456", "654321")
        self.assertEqual(done, b"P\xffbad_code")
        with self.assertRaises(pake.PairError) as ctx:
            client.finish(done)
        self.assertEqual(ctx.exception.code, "bad_code")
        self.assertNotEqual(client.token, board.token)

    def test_code_format(self):
        for bad in ("12345", "1234567", "12345a", ""):
            with self.assertRaises(ValueError):
                pake.code_scalar(bad)

    def test_messages_hide_code_and_credential(self):
        client, board, done = self.run_pair("000123", "000123")
        wire = client.start() + board.reply(client.start())
        self.assertNotIn(b"000123", wire)
        self.assertNotIn(bytes.fromhex(board.token), wire + done)


if __name__ == "__main__":
    unittest.main()
