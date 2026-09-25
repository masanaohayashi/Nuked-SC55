import importlib.util
import tempfile
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("encode.py")
SPEC = importlib.util.spec_from_file_location("panel_sysex_encode", MODULE_PATH)
encode = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(encode)


class PanelSysExEncoderTests(unittest.TestCase):
    def test_pgm_generates_complete_checksum_valid_frame(self):
        pixels = bytes((0, 1, 2, 3)) * (encode.PIXELS // 4)
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "frame.pgm"
            image.write_bytes(b"P5\n# native size\n741 268\n3\n" + pixels)
            self.assertEqual(encode.read_pgm(image), pixels)

        output = encode.encode_pixels(pixels, frame_id=12)
        messages = []
        position = 0
        while position < len(output):
            end = output.index(0xF7, position) + 1
            message = output[position:end]
            self.assertLessEqual(len(message), encode.MAX_MESSAGE_BYTES)
            self.assertEqual(message[0], 0xF0)
            self.assertEqual(message[1], 0x7D)
            self.assertEqual(sum(message[2:-1]) & 0x7F, 0)
            messages.append(message)
            position = end

        chunk_count = (encode.RASTER_BYTES + encode.MAX_CHUNK_BYTES - 1) // encode.MAX_CHUNK_BYTES
        self.assertEqual(len(messages), chunk_count + 2)
        self.assertEqual(messages[0][7], 1)
        self.assertEqual(messages[0][8], 12)
        self.assertTrue(all(message[7] == 2 for message in messages[1:-1]))
        self.assertEqual(messages[-1][7], 3)
        self.assertEqual(messages[-1][8], 12)
        self.assertEqual(messages[1][12], 0b100100)

    def test_clear_packet_and_invalid_dimensions(self):
        clear = encode.encode_clear()
        self.assertEqual(clear[7], 4)
        self.assertEqual(sum(clear[2:-1]) & 0x7F, 0)
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "wrong.pgm"
            image.write_bytes(b"P5\n1 1\n3\n\0")
            with self.assertRaisesRegex(ValueError, "exactly 741x268"):
                encode.read_pgm(image)


if __name__ == "__main__":
    unittest.main()
