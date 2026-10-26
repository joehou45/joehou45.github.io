#!/usr/bin/env python3
"""
I2S-GO 語音助理 — Mac 大腦伺服器 (全本機, 免費, 免金鑰)
  STT : faster-whisper
  LLM : Ollama (qwen2.5:7b)
  TTS : macOS `say` (中文 Meijia) + afconvert -> 16kHz WAV

ESP32 流程:
  POST /chat  body = 原始 PCM (16kHz, 16-bit, mono, little-endian)
      回傳: WAV 音檔 (16kHz/16-bit/mono), 可直接串到 I2S 播放
      標頭: X-STT, X-Reply (UTF-8 百分比編碼, 供除錯/顯示)
  GET  /ping  -> "ok"  (ESP32 開機測試連線用)

跑法:  python3 server.py
"""
import io, os, sys, json, wave, tempfile, subprocess, urllib.request, urllib.parse
from flask import Flask, request, Response

# ---------- 設定 ----------
HOST = "0.0.0.0"
PORT = 5005
SAMPLE_RATE = 16000
OLLAMA_URL = "http://127.0.0.1:11434/api/chat"
OLLAMA_MODEL = "gemma2:9b"      # Google, 非中國
EDGE_VOICE = "zh-TW-HsiaoChenNeural"   # 微軟神經語音, 自然; 男聲=zh-TW-YunJheNeural
import imageio_ffmpeg, datetime
FFMPEG = imageio_ffmpeg.get_ffmpeg_exe()

# ---- 即時資訊 (時間 + 天氣) ----
CITY = "台北"                      # 改成你的城市名 (僅顯示用)
LAT, LON = 25.033, 121.565         # 你的經緯度 (預設台北)
TZ = "Asia/Taipei"
WMO = {0:"晴朗",1:"大致晴朗",2:"局部多雲",3:"陰天",45:"有霧",48:"霧淞",
       51:"毛毛雨",53:"小雨",55:"雨",56:"凍雨",57:"凍雨",61:"小雨",63:"中雨",
       65:"大雨",66:"凍雨",67:"凍雨",71:"小雪",73:"中雪",75:"大雪",77:"雪粒",
       80:"陣雨",81:"陣雨",82:"強陣雨",85:"陣雪",86:"陣雪",95:"雷雨",
       96:"雷雨伴冰雹",99:"強雷雨伴冰雹"}
WEATHER_KW = ["天氣","氣溫","溫度","下雨","會不會雨","會不會下","熱不熱","冷不冷",
              "濕度","出門","帶傘","雨傘","幾度"]

def get_time_context():
    now = datetime.datetime.now()
    wd = "一二三四五六日"[now.weekday()]
    return f"現在時間是 {now.year}年{now.month}月{now.day}日 星期{wd} {now.hour:02d}:{now.minute:02d}。"

def get_weather():
    try:
        url = (f"https://api.open-meteo.com/v1/forecast?latitude={LAT}&longitude={LON}"
               f"&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
               f"weather_code,wind_speed_10m&timezone={urllib.parse.quote(TZ)}")
        with urllib.request.urlopen(url, timeout=8) as r:
            d = json.loads(r.read().decode("utf-8"))["current"]
        desc = WMO.get(d.get("weather_code"), "")
        return (f"{CITY}目前天氣：{desc}，氣溫 {d['temperature_2m']}°C，"
                f"體感 {d['apparent_temperature']}°C，濕度 {d['relative_humidity_2m']}%，"
                f"風速 {d['wind_speed_10m']} 公里。")
    except Exception as e:
        return f"(天氣查詢失敗: {e})"
WHISPER_SIZE = "small"          # tiny/base/small/medium; small 中文較準
SYSTEM_PROMPT = (
    "你的名字叫「小智」，是一個在本機裝置上運作的友善語音助理。"
    "不要宣稱自己來自任何公司或國家；被問到身分時，就說你是小智，一個本機語音助理。"
    "只用繁體中文、口語、親切地回答，絕對不要夾雜任何英文單字或句子。"
    "回答要簡短，控制在 2 句話、40 個字以內，因為你的回答會被唸出來給使用者聽。"
    "\n你可以控制這台裝置的硬體：一顆 RGBW LED 燈 和 嗶聲喇叭。"
    "當使用者要你開/關燈、改顏色、調亮度、或嗶幾聲時，先用口語回覆，"
    "然後『另起一行』只輸出一段 JSON 指令（純聊天時絕對不要輸出 JSON）："
    "燈光用 {\"led\":{\"r\":0-255,\"g\":0-255,\"b\":0-255,\"w\":0-255}}（關燈就全部 0，白燈用 w:255）；"
    "要閃爍就在 led 裡加 \"blink\":次數，例如「紅燈閃三下」用 "
    "{\"led\":{\"r\":255,\"g\":0,\"b\":0,\"w\":0,\"blink\":3}}；"
    "多個顏色依序變換用 {\"seq\":\"r,g,b,w;r,g,b,w;...\"}，"
    "例如「紅轉藍轉綠」用 {\"seq\":\"255,0,0,0;0,0,255,0;0,255,0,0\"}；"
    "嗶聲用 {\"beep\":{\"n\":次數}}；播放音樂用 {\"music\":\"play\"}；需要時可同時給。"
    "這段 JSON 不會被唸出來。例如使用者說「開紅燈」→ 回「好，幫你開紅燈」再換行輸出 "
    "{\"led\":{\"r\":255,\"g\":0,\"b\":0,\"w\":0}}。"
)

app = Flask(__name__)
print(f"[init] loading faster-whisper ({WHISPER_SIZE}) ...", flush=True)
from faster_whisper import WhisperModel
import numpy as np
whisper = WhisperModel(WHISPER_SIZE, device="cpu", compute_type="int8")
print("[init] whisper ready", flush=True)
from opencc import OpenCC
_cc = OpenCC("s2twp")                 # 簡體 -> 繁體(台灣用語)

# ---- TFT 文字渲染 (Mac 畫中文 -> 240x240 RGB565, ESP32 直接貼圖) ----
from PIL import Image, ImageDraw, ImageFont
_FONT_PATH = "/System/Library/Fonts/STHeiti Medium.ttc"
_font = ImageFont.truetype(_FONT_PATH, 28)
last_img = (b"\x00\x00") * (240 * 240)   # 預設黑畫面

def _wrap(draw, text, maxw):
    lines = []
    for para in text.split("\n"):
        cur = ""
        for ch in para:
            if draw.textlength(cur + ch, font=_font) <= maxw:
                cur += ch
            else:
                lines.append(cur); cur = ch
        lines.append(cur)
    return lines

def render_text_rgb565(text):
    text = "".join(c for c in text if ord(c) < 0x10000)   # 去掉 emoji 等
    img = Image.new("RGB", (240, 240), (0, 0, 0))
    d = ImageDraw.Draw(img)
    y = 6
    for ln in _wrap(d, text, 232)[:7]:                    # 最多 7 行
        d.text((6, y), ln, font=_font, fill=(255, 255, 255))
        y += 33
    arr = np.array(img)
    r = (arr[:, :, 0].astype(np.uint16) >> 3)
    g = (arr[:, :, 1].astype(np.uint16) >> 2)
    b = (arr[:, :, 2].astype(np.uint16) >> 3)
    return ((r << 11) | (g << 5) | b).astype(">u2").tobytes()

last_mono = b"\x00" * (240 * 30)        # 1-bit 單色點陣 (240x240, 每列 30 bytes)

def render_text_mono(text):
    text = "".join(c for c in text if ord(c) < 0x10000)
    img = Image.new("L", (240, 240), 0)
    d = ImageDraw.Draw(img)
    y = 6
    for ln in _wrap(d, text, 232)[:7]:
        d.text((6, y), ln, font=_font, fill=255)
        y += 33
    bits = (np.array(img) > 110).astype(np.uint8)   # 240x240 0/1
    return np.packbits(bits, axis=1).tobytes()      # 240 x 30 bytes, MSB first


def transcribe(pcm_bytes):
    audio = np.frombuffer(pcm_bytes, dtype=np.int16).astype(np.float32) / 32768.0
    segments, info = whisper.transcribe(audio, language="zh", beam_size=1, vad_filter=True)
    text = "".join(s.text for s in segments).strip()
    return _cc.convert(text)          # 統一轉繁體 (關鍵字比對/模型輸入都一致)


def ask_llm(user_text, context="", history=None):
    sys_content = SYSTEM_PROMPT
    if context:
        sys_content += "\n\n【以下是目前的即時資訊，回答相關問題時請直接參考，不要說你不知道】\n" + context
    messages = [{"role": "system", "content": sys_content}]
    if history:
        messages += history                       # 帶入最近幾輪對話
    messages.append({"role": "user", "content": user_text})
    payload = {
        "model": OLLAMA_MODEL,
        "messages": messages,
        "stream": False,
        "options": {"temperature": 0.6, "num_predict": 150, "num_ctx": 8192},
    }
    req = urllib.request.Request(
        OLLAMA_URL, data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=60) as r:
        data = json.loads(r.read().decode("utf-8"))
    return data["message"]["content"].strip()


def tts_wav(text):
    """用 edge-tts 神經語音產生中文語音, 轉成 16kHz/16-bit/mono WAV, 回傳 bytes。"""
    with tempfile.TemporaryDirectory() as d:
        mp3 = os.path.join(d, "r.mp3")
        wav = os.path.join(d, "r.wav")
        subprocess.run(["edge-tts", "--voice", EDGE_VOICE, "--text", text,
                        "--write-media", mp3], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # -map_metadata -1 + -fflags +bitexact: 不寫 LIST/INFO metadata,
        # 讓 WAV 標頭維持標準 44 bytes (ESP32 固定跳 44 才正確)
        subprocess.run([FFMPEG, "-y", "-i", mp3, "-ar", str(SAMPLE_RATE),
                        "-ac", "1", "-acodec", "pcm_s16le",
                        "-map_metadata", "-1", "-fflags", "+bitexact",
                        "-f", "wav", wav],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(wav, "rb") as f:
            return f.read()


# ---- 硬體動作解析 (LLM 輸出 JSON 為主, 關鍵字規則為後援) ----
import re as _re
_COLORS = {"紅": (255, 0, 0, 0), "綠": (0, 255, 0, 0), "藍": (0, 0, 255, 0),
           "黃": (255, 170, 0, 0), "白": (0, 0, 0, 255), "紫": (190, 0, 255, 0),
           "橙": (255, 70, 0, 0), "橘": (255, 70, 0, 0), "青": (0, 255, 255, 0),
           "粉": (255, 50, 120, 0), "暖白": (120, 80, 20, 255)}

_CN = {"一": 1, "二": 2, "兩": 2, "三": 3, "四": 4, "五": 5,
       "六": 6, "七": 7, "八": 8, "九": 9, "十": 10}
_BEEP_RE = _re.compile(r"(嗶|逼|必|畢|比|閉|碧|壁|蜂鳴|鈴|beep)", _re.I)

def _num(s, default=1):
    m = _re.search(r"(\d+)", s)
    if m:
        return int(m.group(1))
    for c, v in _CN.items():
        if c in s:
            return v
    return default

def _color_hits(stt):
    """依出現順序找出有『動作語境』的顏色 (避免純聊天誤觸發)。"""
    hits = []
    for n in _COLORS:
        for ctx in (n + "燈", "開" + n, "變" + n, "轉" + n, "換" + n, "亮" + n):
            p = stt.find(ctx)
            if p >= 0:
                hits.append((p, n)); break
    return [n for _, n in sorted(hits)]

def rule_action(stt):
    a = {}
    if any(k in stt for k in ["關燈", "關掉燈", "熄燈", "把燈關", "關閉燈", "燈關掉", "關燈光"]):
        a["led"] = {"r": 0, "g": 0, "b": 0, "w": 0}
    else:
        names = _color_hits(stt)
        if len(names) >= 2:            # 多色 -> 序列
            a["seq"] = ";".join("%d,%d,%d,%d" % _COLORS[n] for n in names)
        elif len(names) == 1:
            r, g, b, w = _COLORS[names[0]]
            a["led"] = {"r": r, "g": g, "b": b, "w": w}
        elif any(k in stt for k in ["開燈", "亮燈", "把燈打開", "開啟燈", "點燈"]):
            a["led"] = {"r": 0, "g": 0, "b": 0, "w": 255}
    # 閃爍: 有「閃/眨」就加 blink 次數 (沒指定顏色就預設白)
    if any(k in stt for k in ["閃", "眨"]):
        if "led" not in a:
            a["led"] = {"r": 0, "g": 0, "b": 0, "w": 255}
        a["led"]["blink"] = max(1, min(10, _num(stt, 3)))
    # 嗶聲: 明確 beep 字 或 (同音字 + 聲/下/次)
    if _BEEP_RE.search(stt) and ("聲" in stt or "下" in stt or "次" in stt or "嗶" in stt or "beep" in stt.lower()):
        a["beep"] = {"n": max(1, min(10, _num(stt)))}
    # 播放音樂
    if any(k in stt for k in ["播放音樂", "放音樂", "放首歌", "來點音樂", "聽音樂", "播音樂", "放歌", "來首歌"]) \
       or ("音樂" in stt and any(k in stt for k in ["播", "放", "聽", "來"])):
        a["music"] = "play"
    return a

def _clean_text(t):
    t = _re.sub(r"```[a-zA-Z]*", "", t)     # 去掉 ```json / ``` 圍欄
    t = t.replace("`", "")
    return t.strip().strip("\n").strip()

# 由回覆文字判斷心情, 傳給 ESP32 畫對應表情 (ASCII tag)
_MOOD_KW = {
    "love":      ["愛你", "喜歡你", "抱抱", "么么", "想你", "親親", "❤", "💕", "😘", "最棒的你"],
    "happy":     ["哈哈", "呵呵", "開心", "太好", "好耶", "耶", "讚", "棒", "恭喜", "太棒", "高興", "😄", "😁", "😆", "🎉", "好棒", "嘻嘻"],
    "surprised": ["哇", "天啊", "天哪", "真的嗎", "竟然", "居然", "不會吧", "太驚人", "嚇", "😮", "😲", "！？", "好厲害"],
    "sad":       ["抱歉", "對不起", "難過", "傷心", "可惜", "遺憾", "唉", "嗚", "不好意思", "辛苦", "別難過", "😢", "😭", "哭"],
    "angry":     ["生氣", "討厭", "可惡", "哼", "不要", "煩", "氣死", "😠", "😡", "不准"],
}
def mood_of(text):
    for mood, kws in _MOOD_KW.items():
        if any(k in text for k in kws):
            return mood
    if "？" in text or "?" in text:             # 反問多半是好奇/驚訝
        return "surprised"
    return "neutral"

def parse_action(reply, stt):
    """抽 LLM JSON 動作 + 關鍵字規則互補合併; 回傳(乾淨口語, 動作dict或None)。"""
    action, clean = {}, reply
    i = reply.find("{")
    if i >= 0:
        j = reply.rfind("}")
        if j > i:
            try:
                obj = json.loads(reply[i:j + 1])
                if isinstance(obj, dict) and ("led" in obj or "beep" in obj or "seq" in obj or "music" in obj):
                    action = obj
                    clean = reply[:i]            # JSON 之前才是口語
            except Exception:
                pass
    clean = _clean_text(clean)                   # 清掉 markdown 圍欄等殘留
    r_action = rule_action(stt)
    if "seq" in r_action:                        # 多色序列優先 (蓋掉單色 led)
        action.pop("led", None)
        action["seq"] = r_action["seq"]
    if "led" in r_action:                        # 規則顏色較準, 覆蓋 LLM; 但保留 LLM 的 blink
        if action.get("led", {}).get("blink") and "blink" not in r_action["led"]:
            r_action["led"]["blink"] = action["led"]["blink"]
        action["led"] = r_action["led"]
    for k, v in r_action.items():                # 其餘 (beep 等) 補上
        action.setdefault(k, v)
    return clean, (action or None)


# ---- 多輪對話記憶 ----
MAX_TURNS = 6                        # 保留最近 6 問 6 答
RESET_KW = ["重新開始", "清除記憶", "忘記剛才", "忘掉剛才", "重來", "重置"]
conversation = []                    # [{role, content}, ...] 只存 user / assistant


@app.route("/ping")
def ping():
    return "ok"


@app.route("/textimg")
def textimg():
    # 回傳最後一次回覆渲染好的 240x240 RGB565 (115200 bytes)
    return Response(bytes(last_img), mimetype="application/octet-stream")


@app.route("/textmono")
def textmono():
    # 回傳最後一次回覆的 1-bit 單色點陣 (7200 bytes), 供 ESP32 逐字顯示
    return Response(bytes(last_mono), mimetype="application/octet-stream")


@app.route("/reset", methods=["GET", "POST"])
def reset():
    conversation.clear()
    return "memory cleared"


# 網路電台 (可改) ; 用 ffmpeg 即時轉 44.1kHz 單聲道 PCM 串流給 ESP32 (音質較好)
RADIO_URL = "http://ice1.somafm.com/groovesalad-128-mp3"
MUSIC_RATE = 44100

@app.route("/music")
def music():
    url = request.args.get("url", RADIO_URL)
    def gen():
        proc = subprocess.Popen(
            [FFMPEG, "-nostdin", "-i", url, "-ar", str(MUSIC_RATE),
             "-ac", "1", "-f", "s16le", "-"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        try:
            while True:
                chunk = proc.stdout.read(4096)
                if not chunk:
                    break
                yield chunk
        finally:
            proc.kill()
    return Response(gen(), mimetype="application/octet-stream")


@app.route("/chat", methods=["POST"])
def chat():
    pcm = request.get_data()
    if len(pcm) < 1000:
        return Response("audio too short", status=400)
    action = None
    try:
        stt = transcribe(pcm)
        print(f"[STT] {stt}", flush=True)
        if not stt:
            stt = "(聽不清楚)"
            reply = "抱歉，我沒聽清楚，可以再說一次嗎？"
        elif any(k in stt for k in RESET_KW):
            conversation.clear()
            reply = "好的，我們重新開始吧。"
        else:
            ctx = get_time_context()
            if any(k in stt for k in WEATHER_KW):
                ctx += " " + get_weather()
            print(f"[CTX] {ctx}", flush=True)
            reply = _cc.convert(ask_llm(stt, ctx, history=conversation))
            reply, action = parse_action(reply, stt)   # 抽出硬體動作, 去掉 JSON
            if not reply:
                reply = "好的。"
            print(f"[ACTION] {action}", flush=True)
            conversation.append({"role": "user", "content": stt})
            conversation.append({"role": "assistant", "content": reply})
            del conversation[: max(0, len(conversation) - MAX_TURNS * 2)]
        reply = _cc.convert(reply)        # 回覆也統一繁體
        print(f"[LLM] {reply}", flush=True)
        global last_img, last_mono
        last_img = render_text_rgb565(reply)   # 供 /textimg
        last_mono = render_text_mono(reply)    # 供 /textmono (逐字顯示)
        wav = tts_wav(reply)
    except Exception as e:
        print("[error]", e, flush=True)
        return Response(f"error: {e}", status=500)

    resp = Response(wav, mimetype="audio/wav")
    resp.headers["X-STT"] = urllib.parse.quote(stt)
    resp.headers["X-Reply"] = urllib.parse.quote(reply)
    resp.headers["X-Mood"] = mood_of(reply)       # 心情: happy/sad/angry/surprised/love/neutral
    if action:
        resp.headers["X-Action"] = json.dumps(action, ensure_ascii=True)   # ASCII JSON, ESP32 直接讀
    return resp


if __name__ == "__main__":
    print(f"[init] serving on http://{HOST}:{PORT}  (LAN: 192.168.68.126:{PORT})", flush=True)
    app.run(host=HOST, port=PORT, threaded=True)
