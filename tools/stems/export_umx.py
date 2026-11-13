# Writes each UMX-HQ target as a flat little-endian float32 file for the C separator, with
# batch norms folded into a scale and a shift. Also makes a test clip and PyTorch references.
import struct, sys, numpy as np, torch
from openunmix import utils, model as umodel

TARGETS = ["drums", "bass", "vocals", "other"]
sep = utils.load_separator("umxhq", targets=TARGETS, niter=0, device="cpu")
sep.eval()

def fold(bn):
    a = bn.weight.detach() / torch.sqrt(bn.running_var + bn.eps)
    c = bn.bias.detach() - bn.running_mean * a
    return a.numpy(), c.numpy()

for t in TARGETS:
    m = sep.target_models[t]
    sd = {k: v.detach().float() for k, v in m.state_dict().items()}
    out = open(f"umxhq-{t}.bin", "wb")
    out.write(b"UMX1")
    out.write(struct.pack("<iii", m.nb_bins, m.hidden_size, m.nb_output_bins))
    def w(x): out.write(np.ascontiguousarray(np.asarray(x, dtype="<f4")).tobytes())
    w(sd["input_mean"]); w(sd["input_scale"])
    w(sd["fc1.weight"]); w(fold(m.bn1)[0]); w(fold(m.bn1)[1])
    for layer in range(3):
        for sfx in ["", "_reverse"]:
            w(sd[f"lstm.weight_ih_l{layer}{sfx}"]); w(sd[f"lstm.weight_hh_l{layer}{sfx}"])
            w(sd[f"lstm.bias_ih_l{layer}{sfx}"] + sd[f"lstm.bias_hh_l{layer}{sfx}"])
    w(sd["fc2.weight"]); w(fold(m.bn2)[0]); w(fold(m.bn2)[1])
    w(sd["fc3.weight"]); w(fold(m.bn3)[0]); w(fold(m.bn3)[1])
    w(sd["output_scale"]); w(sd["output_mean"])
    out.close()
    print("wrote", t)

# A 15 s stereo test clip: kick, snare, hats, a bass line, chords and a sung-like tone.
sr, n = 44100, 44100 * 15
tt = np.arange(n) / sr
rng = np.random.default_rng(1)
x = np.zeros((2, n))
beat = 0.5
for b in range(30):
    s = int(b * beat * sr)
    k = np.arange(int(0.25 * sr)) / sr
    x[:, s:s + len(k)] += 0.8 * np.sin(2 * np.pi * (50 + 80 * np.exp(-k * 30)) * k) * np.exp(-k * 12)
    if b % 2:
        x[:, s:s + 6000] += 0.3 * rng.standard_normal(6000) * np.exp(-np.arange(6000) / 1500)
    for h in range(2):
        hs = s + h * int(beat / 2 * sr)
        x[:, hs:hs + 1500] += 0.08 * rng.standard_normal(1500) * np.exp(-np.arange(1500) / 300)
bassnotes = [41.2, 41.2, 55.0, 49.0]
for i, f in enumerate(np.repeat(bassnotes, 1)):
    pass
f_bass = np.array([bassnotes[int(t0 / 2) % 4] for t0 in tt])
x += 0.3 * np.sign(np.sin(2 * np.pi * np.cumsum(f_bass) / sr)) * 0.5
chord = sum(np.sin(2 * np.pi * f * tt) for f in [261.6, 329.6, 392.0])
x[0] += 0.08 * chord; x[1] += 0.08 * chord * 0.8
voice = np.sin(2 * np.pi * (440 + 6 * np.sin(2 * np.pi * 5.5 * tt)) * tt) * (0.5 + 0.5 * np.sin(2 * np.pi * 0.25 * tt))
x += 0.12 * voice
x = (x / np.max(np.abs(x)) * 0.9).astype(np.float32)

def wav(path, y):
    y = np.clip(y, -1, 1)
    pcm = (y.T * 32767).astype("<i2").tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 2, sr, sr * 4, 4, 16))
        f.write(b"data" + struct.pack("<I", len(pcm)) + pcm)

wav("stemtest.wav", x)
# References from the clip as the C code will read it (16-bit), one target at a time, no EM.
xq = np.round(np.clip(x, -1, 1) * 32767) / 32767
audio = torch.from_numpy(xq.astype(np.float32))[None]
for t in TARGETS:
    one = umodel.Separator({t: sep.target_models[t]}, niter=0, residual=False, sample_rate=44100, n_fft=4096, n_hop=1024, nb_channels=2)
    one.eval()
    with torch.no_grad():
        est = one(audio)[0, 0].numpy()
    np.save(f"ref-{t}.npy", est)
    print("ref", t, est.shape)
