from machine import I2S, Pin
import math, struct, time

RATE=16000; FREQ=880; AMP=8000
audio = I2S(0, sck=Pin(13), ws=Pin(12), sd=Pin(15),
            mode=I2S.TX, bits=16, format=I2S.STEREO, rate=RATE, ibuf=20000)

def make_tone(freq, ms, amp):
    n = RATE*ms//1000
    buf = bytearray(n*4)
    for i in range(n):
        s = int(math.sin(2*math.pi*freq*i/RATE)*amp)
        struct.pack_into('<hh', buf, i*4, s, s)
    return buf

tone = make_tone(FREQ,150,AMP)
sil  = bytearray(RATE*200//1000*4)
print("playing 10 beeps...")
for b in range(10):
    print("beep", b+1)
    audio.write(tone)
    audio.write(sil)
audio.deinit()
print("done 10 beeps")
