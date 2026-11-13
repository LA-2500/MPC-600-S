import numpy as np, struct, sys
def wav(path):
    d = open(path, "rb").read()
    i = d.index(b"data") + 8
    return np.frombuffer(d[i:], "<i2").astype(np.float32).reshape(-1, 2).T / 32767
for t in ["drums", "bass", "vocals", "other"]:
    ref = np.load(f"ref-{t}.npy")
    c = wav(f"stem-{t}.wav")
    n = min(ref.shape[1], c.shape[1])
    err = c[:, :n] - np.clip(ref[:, :n], -1, 1)
    snr = 10 * np.log10(np.sum(ref[:, :n] ** 2) / max(np.sum(err ** 2), 1e-20))
    print(f"{t:7s} match vs PyTorch: {snr:5.1f} dB SNR   (peak diff {np.max(np.abs(err)):.5f}, stem level {np.sqrt(np.mean(ref**2)):.4f})")
mix = wav("stemtest.wav"); voc = np.load("ref-vocals.npy"); mus = wav("stem-music.wav")
n = min(mix.shape[1], mus.shape[1])
err = mus[:, :n] - (mix[:, :n] - voc[:, :n])
print(f"music   vs mix - vocals: {10*np.log10(np.sum((mix[:, :n]-voc[:, :n])**2)/np.sum(err**2)):5.1f} dB SNR")
