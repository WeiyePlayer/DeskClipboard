import unittest

from clipboard_agent.extract import extract_chips


class ExtractTest(unittest.TestCase):
    def test_phone_url_and_code(self):
        text = "四川省成都市郫都区欣茂西郡兰庭 张世伦 13558801181"
        self.assertEqual(extract_chips(text), ["13558801181"])
        link = "https://detail.tmall.com/item.htm?abbucket=15"
        self.assertEqual(extract_chips(link), [link])
        self.assertEqual(extract_chips("验证码 5821 发给 a@b.cn"), ["5821", "a@b.cn"])

    def test_caps_at_three_and_ignores_url_digits(self):
        text = "验证码 1234 https://a.com/1 13900001111 user@example.com 13800002222"
        chips = extract_chips(text)
        self.assertEqual(len(chips), 3)
        self.assertIn("https://a.com/1", chips)


if __name__ == "__main__":
    unittest.main()
