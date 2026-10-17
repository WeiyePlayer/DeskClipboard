import io
import time
import unittest
from unittest.mock import patch

from PIL import Image

from clipboard_agent import render
from clipboard_agent.pillow_canvas import PillowCanvas, text_width
import test_render


class PillowRenderTest(test_render.RenderTest):
    """Exercise the Windows drawing implementation on the current host."""
    def setUp(self):
        self.canvas = patch.object(render, "_Canvas", PillowCanvas)
        self.measure = patch.object(render, "_text_width", text_width)
        self.canvas.start(); self.measure.start()
        self.addCleanup(self.canvas.stop);self.addCleanup(self.measure.stop)

    def test_two_lines_and_large_image_metadata_fit(self):
        canvas=PillowCanvas(224,60,(255,255,255))
        canvas.text("第一行\n第二行",10,5,180,40,15,(0,0,0))
        self.assertNotEqual(canvas.picture.crop((10,25,190,45)).getextrema(),((255,255),)*3)
        start=time.monotonic()
        render.render_text_card("中"*21000)
        self.assertLess(time.monotonic()-start,5)
        stream=io.BytesIO();Image.new("RGBA",(4,4),(0,0,0,0)).save(stream,"PNG")
        card=render.render_image_card(stream.getvalue(),65535,65535)
        self.assertEqual(card.height,142)
        # Transparent pixels blend into white; the page corner stays gray.
        p=lambda x,y:int.from_bytes(card.rgb565[(y*card.width+x)*2:(y*card.width+x)*2+2],"little")
        self.assertEqual(p(20,20),65535)
        self.assertNotEqual(p(0,0),65535)


if __name__=="__main__":unittest.main()
