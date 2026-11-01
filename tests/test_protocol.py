import sys
from pathlib import Path
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'shared'/'protocol'))
from lx_protocol import *

class ProtocolTests(unittest.TestCase):
    def test_split_corrupt_recover(self):
        p=Packet(KEYBOARD,0x12345678,0x10203040,bytes(range(1,9)))
        encoded=encode(p)
        self.assertEqual(encoded.hex(),'4c5802010800785634124030201001020304050607083be3')
        self.assertEqual(decode(encoded),p)
        for cut in range(len(encoded)+1):
            stream=StreamDecoder()
            self.assertEqual(stream.feed(encoded[:cut])+stream.feed(encoded[cut:]),[p])
        corrupt=bytearray(encoded);corrupt[14]^=1
        stream=StreamDecoder()
        self.assertEqual(stream.feed(b'noise'+corrupt+encoded),[p])
        for end in range(len(encoded)):
            with self.assertRaises(ValueError):decode(encoded[:end])
    def test_bounds_and_multiple(self):
        p=Packet(TEXT,0xffffffff,1,b'x'*234)
        self.assertEqual(len(encode(p)),250)
        with self.assertRaises(ValueError):encode(Packet(TEXT,0,0,b'x'*235))
        stream=StreamDecoder()
        self.assertEqual(stream.feed(encode(p)*3),[p]*3)
        stream.feed(b'x'*10000)
        self.assertLess(len(stream.buffer),250)

if __name__=='__main__':unittest.main()
