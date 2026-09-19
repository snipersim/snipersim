#!/usr/bin/env python3
"""Train, on the host, the tiny tool-using GPT that test/llm-agent/agent.c runs
on the RISC-V infrastructure.  numpy only; nothing here runs on the edge.

The task is a character-level "agent" protocol.  A transcript is

    Q: what is 12 plus 7?\\n      the question
    T: add 12 7\\n                the model's tool call
    R: 19\\n                      the runtime's tool result
    A: 19\\n                      the model's answer
    .                            end marker

Tools: add, mul (two integers) and len (a word).  The model learns to turn
the question into the call and to copy the result into the answer; the
runtime (agent.c) executes the call.  The architecture is nanoGPT's: token
and position embeddings, pre-LN transformer blocks with causal
self-attention and a GELU MLP, final LN, a linear head.  Manual backprop,
Adam, float32 throughout so the exported weights and the C inference agree.

    train_agent_gpt.py --out test/llm-agent [--steps N] [--seed S]

writes model_weights.h (weights, shape constants, vocabulary) and
prompts.h (test prompts with their expected answers, verified on the
numpy model with greedy decoding before export)."""
import argparse, os, sys, time
import numpy as np

# ------------------------------------------------------------------ corpus
WORDS = ['cat', 'tree', 'house', 'apple', 'river', 'stone', 'cloud', 'bridge', 'garden', 'window']

def make_example(rng):
    kind = rng.integers(3)
    if kind == 0:
        a, b = int(rng.integers(100)), int(rng.integers(100))
        return 'Q: what is %d plus %d?\nT: add %d %d\nR: %d\nA: %d\n.' % (a, b, a, b, a + b, a + b)
    if kind == 1:
        a, b = int(rng.integers(13)), int(rng.integers(13))
        return 'Q: what is %d times %d?\nT: mul %d %d\nR: %d\nA: %d\n.' % (a, b, a, b, a * b, a * b)
    w = WORDS[rng.integers(len(WORDS))]
    return 'Q: how long is %s?\nT: len %s\nR: %d\nA: %d\n.' % (w, w, len(w), len(w))

VOCAB = sorted(set(''.join(make_example(np.random.default_rng(i)) for i in range(500)) + 'abcdefghijklmnopqrstuvwxyz0123456789 ?:.\n'))
STOI = {c: i for i, c in enumerate(VOCAB)}

# ------------------------------------------------------------------ model
class GPT:
    def __init__(self, V, T, d, L, H, rng):
        self.V, self.T, self.d, self.L, self.H = V, T, d, L, H
        s = 0.02
        p = {'wte': rng.standard_normal((V, d)) * s, 'wpe': rng.standard_normal((T, d)) * s,
             'lnf_g': np.ones(d), 'lnf_b': np.zeros(d), 'head': rng.standard_normal((d, V)) * s}
        for l in range(L):
            p.update({'ln1_g%d' % l: np.ones(d), 'ln1_b%d' % l: np.zeros(d),
                      'wqkv%d' % l: rng.standard_normal((d, 3 * d)) * s, 'bqkv%d' % l: np.zeros(3 * d),
                      'wo%d' % l: rng.standard_normal((d, d)) * s / np.sqrt(2 * L), 'bo%d' % l: np.zeros(d),
                      'ln2_g%d' % l: np.ones(d), 'ln2_b%d' % l: np.zeros(d),
                      'w1%d' % l: rng.standard_normal((d, 4 * d)) * s, 'b1%d' % l: np.zeros(4 * d),
                      'w2%d' % l: rng.standard_normal((4 * d, d)) * s / np.sqrt(2 * L), 'b2%d' % l: np.zeros(d)})
        self.p = {k: v.astype(np.float32) for k, v in p.items()}

    # -- pieces, each returning (out, cache)
    @staticmethod
    def ln(x, g, b, eps=1e-5):
        mu = x.mean(-1, keepdims=True); xc = x - mu; var = (xc * xc).mean(-1, keepdims=True)
        rstd = 1.0 / np.sqrt(var + eps); xh = xc * rstd
        return xh * g + b, (xh, rstd, g)
    @staticmethod
    def ln_back(dy, cache):
        xh, rstd, g = cache
        dg = (dy * xh).sum(axis=tuple(range(dy.ndim - 1))); db = dy.sum(axis=tuple(range(dy.ndim - 1)))
        dxh = dy * g
        dx = rstd * (dxh - dxh.mean(-1, keepdims=True) - xh * (dxh * xh).mean(-1, keepdims=True))
        return dx, dg, db
    @staticmethod
    def gelu(x):
        # tanh approximation, as agent.c implements it
        u = 0.7978845608028654 * (x + 0.044715 * x ** 3); t = np.tanh(u)
        return 0.5 * x * (1 + t), (x, t)
    @staticmethod
    def gelu_back(dy, cache):
        x, t = cache
        du = 0.7978845608028654 * (1 + 3 * 0.044715 * x * x)
        return dy * (0.5 * (1 + t) + 0.5 * x * (1 - t * t) * du)

    def forward(self, idx, targets=None):
        p, B, T, d, H = self.p, idx.shape[0], idx.shape[1], self.d, self.H
        hd = d // H
        x = p['wte'][idx] + p['wpe'][:T]
        caches = []
        mask = np.tril(np.ones((T, T), dtype=bool))
        for l in range(self.L):
            h1, c1 = self.ln(x, p['ln1_g%d' % l], p['ln1_b%d' % l])
            qkv = h1 @ p['wqkv%d' % l] + p['bqkv%d' % l]
            q, k, v = [a.reshape(B, T, H, hd).transpose(0, 2, 1, 3) for a in np.split(qkv, 3, axis=-1)]
            att = (q @ k.transpose(0, 1, 3, 2)) / np.sqrt(hd)
            att = np.where(mask, att, -1e9)
            att = att - att.max(-1, keepdims=True); att = np.exp(att); att = att / att.sum(-1, keepdims=True)
            y = (att @ v).transpose(0, 2, 1, 3).reshape(B, T, d)
            o = y @ p['wo%d' % l] + p['bo%d' % l]
            x2 = x + o
            h2, c2 = self.ln(x2, p['ln2_g%d' % l], p['ln2_b%d' % l])
            m1 = h2 @ p['w1%d' % l] + p['b1%d' % l]
            g, cg = self.gelu(m1)
            m2 = g @ p['w2%d' % l] + p['b2%d' % l]
            x3 = x2 + m2
            caches.append((x, h1, c1, q, k, v, att, y, x2, h2, c2, m1, cg, g))
            x = x3
        hf, cf = self.ln(x, p['lnf_g'], p['lnf_b'])
        logits = hf @ p['head']
        if targets is None:
            return logits
        lg = logits - logits.max(-1, keepdims=True); ex = np.exp(lg); probs = ex / ex.sum(-1, keepdims=True)
        n = B * T
        loss = -np.log(probs.reshape(n, -1)[np.arange(n), targets.reshape(-1)] + 1e-12).mean()
        # ---- backward
        grads = {k: np.zeros_like(v) for k, v in p.items()}
        dlogits = probs.copy(); dlogits.reshape(n, -1)[np.arange(n), targets.reshape(-1)] -= 1; dlogits /= n
        grads['head'] += (hf.reshape(n, d).T @ dlogits.reshape(n, -1))
        dhf = dlogits @ p['head'].T
        dx, grads['lnf_g'], grads['lnf_b'] = self.ln_back(dhf, cf)
        for l in reversed(range(self.L)):
            xin, h1, c1, q, k, v, att, y, x2, h2, c2, m1, cg, g = caches[l]
            dm2 = dx
            grads['w2%d' % l] += g.reshape(n, -1).T @ dm2.reshape(n, d); grads['b2%d' % l] += dm2.reshape(n, d).sum(0)
            dg = dm2 @ p['w2%d' % l].T
            dm1 = self.gelu_back(dg, cg)
            grads['w1%d' % l] += h2.reshape(n, d).T @ dm1.reshape(n, -1); grads['b1%d' % l] += dm1.reshape(n, -1).sum(0)
            dh2 = dm1 @ p['w1%d' % l].T
            dx2, grads['ln2_g%d' % l], grads['ln2_b%d' % l] = self.ln_back(dh2, c2)
            dx2 = dx2 + dx
            do = dx2
            grads['wo%d' % l] += y.reshape(n, d).T @ do.reshape(n, d); grads['bo%d' % l] += do.reshape(n, d).sum(0)
            dy = (do @ p['wo%d' % l].T).reshape(B, T, H, hd).transpose(0, 2, 1, 3)
            datt = dy @ v.transpose(0, 1, 3, 2)
            dv = att.transpose(0, 1, 3, 2) @ dy
            ds = att * (datt - (datt * att).sum(-1, keepdims=True))
            ds = np.where(mask, ds, 0) / np.sqrt(hd)
            dq = ds @ k; dk = ds.transpose(0, 1, 3, 2) @ q
            dqkv = np.concatenate([a.transpose(0, 2, 1, 3).reshape(B, T, d) for a in (dq, dk, dv)], axis=-1)
            grads['wqkv%d' % l] += h1.reshape(n, d).T @ dqkv.reshape(n, -1); grads['bqkv%d' % l] += dqkv.reshape(n, -1).sum(0)
            dh1 = dqkv @ p['wqkv%d' % l].T
            dxin, grads['ln1_g%d' % l], grads['ln1_b%d' % l] = self.ln_back(dh1, c1)
            dx = dxin + dx2
        grads['wpe'][:T] += dx.sum(0)
        np.add.at(grads['wte'], idx, dx)
        return loss, grads

    def generate(self, prompt, stop='\n', max_new=None):
        max_new = GEN_MAX if max_new is None else max_new
        ids = [STOI[c] for c in prompt]
        out = ''
        for _ in range(max_new):
            if len(ids) >= self.T:       # the runtime never slides the window: it stops at its end
                break
            ctx = np.array([ids])
            logits = self.forward(ctx)
            nxt = int(np.argmax(logits[0, -1]))
            c = VOCAB[nxt]
            ids.append(nxt); out += c
            if c == stop:
                break
        return out

# ------------------------------------------------------------------ tools (the runtime's, mirrored here)
GEN_MAX = 24     # tokens per generate: what agent.c's GEN_CALL/GEN_ANS must match for an untrained model

def run_tool(call):
    parts = call.split()
    if parts[0] == 'add': return str(int(parts[1]) + int(parts[2]))
    if parts[0] == 'mul': return str(int(parts[1]) * int(parts[2]))
    if parts[0] == 'len': return str(len(parts[1]))
    return '?'

def agent_episode(model, question):
    """The protocol agent.c runs: prompt, tool call, tool, answer."""
    ctx = 'Q: %s\nT: ' % question
    call = model.generate(ctx).rstrip('\n')
    ctx += call + '\nR: %s\nA: ' % run_tool(call)
    ans = model.generate(ctx).rstrip('\n')
    return call, ans

def grad_check(rng):
    m = GPT(len(VOCAB), 8, 16, 1, 2, rng)
    m.p = {k: v.astype(np.float64) for k, v in m.p.items()}
    idx = rng.integers(len(VOCAB), size=(2, 8)); tgt = rng.integers(len(VOCAB), size=(2, 8))
    _, g = m.forward(idx, tgt)
    worst = 0
    for name in ('wqkv0', 'ln1_g0', 'w2_0'.replace('_', ''), 'wte', 'wpe', 'head', 'bqkv0', 'ln2_b0'):
        for _ in range(3):
            i = tuple(rng.integers(s) for s in m.p[name].shape)
            old = m.p[name][i]; eps = 1e-5
            m.p[name][i] = old + eps; lp, _ = m.forward(idx, tgt)
            m.p[name][i] = old - eps; lm, _ = m.forward(idx, tgt)
            m.p[name][i] = old
            num = (lp - lm) / (2 * eps); ana = g[name][i]
            worst = max(worst, abs(num - ana) / max(1e-8, abs(num) + abs(ana)))
    return worst

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', default='test/llm-agent'); ap.add_argument('--steps', type=int, default=6000)
    ap.add_argument('--seed', type=int, default=1); ap.add_argument('--d', type=int, default=64)
    ap.add_argument('--layers', type=int, default=3); ap.add_argument('--heads', type=int, default=4)
    ap.add_argument('--ctx', type=int, default=64); ap.add_argument('--batch', type=int, default=64)
    ap.add_argument('--export-anyway', action='store_true', help='export even an untrained model (prompts carry its own outputs)')
    ap.add_argument('--blob', action='store_true', help='write the weights as model_weights.bin (.incbin) instead of C text')
    ap.add_argument('--eval-questions', type=int, default=40, help='fresh questions to evaluate the agent protocol on')
    ap.add_argument('--prompts', type=int, default=8, help='prompts to export')
    ap.add_argument('--gen', type=int, default=24, help='tokens per generate (numpy and, via GEN_CALL/GEN_ANS, agent.c)')
    a = ap.parse_args()
    global GEN_MAX
    GEN_MAX = a.gen
    rng = np.random.default_rng(a.seed)
    print('vocab %d chars; gradient check (float64, relative error): %.2e' % (len(VOCAB), grad_check(np.random.default_rng(0))))
    model = GPT(len(VOCAB), a.ctx, a.d, a.layers, a.heads, rng)
    nparam = sum(v.size for v in model.p.values())
    print('model: d=%d L=%d H=%d ctx=%d, %d parameters (%.0f KB float32)' % (a.d, a.layers, a.heads, a.ctx, nparam, nparam * 4 / 1024))
    # Adam
    mom = {k: np.zeros_like(v) for k, v in model.p.items()}; vel = {k: np.zeros_like(v) for k, v in model.p.items()}
    lr0, b1, b2 = 3e-3, 0.9, 0.99
    t0 = time.time()
    for step in range(1, a.steps + 1):
        # a batch of transcripts, each cropped/padded to ctx+1 tokens (pad with '.' which is also the end marker)
        seqs = []
        for _ in range(a.batch):
            s = make_example(rng)
            s = (s + '.' * (a.ctx + 1))[:a.ctx + 1]
            seqs.append([STOI[c] for c in s])
        seqs = np.array(seqs)
        loss, grads = model.forward(seqs[:, :-1], seqs[:, 1:])
        lr = lr0 * (0.5 * (1 + np.cos(np.pi * step / a.steps))) + 1e-4
        for k in model.p:
            gk = grads[k].astype(np.float32)
            mom[k] = b1 * mom[k] + (1 - b1) * gk; vel[k] = b2 * vel[k] + (1 - b2) * gk * gk
            mh = mom[k] / (1 - b1 ** step); vh = vel[k] / (1 - b2 ** step)
            model.p[k] = (model.p[k] - lr * mh / (np.sqrt(vh) + 1e-8)).astype(np.float32)
        if step % 200 == 0 or step == 1:
            print('step %5d  loss %.4f  lr %.2e  %.0fs' % (step, loss, lr, time.time() - t0)); sys.stdout.flush()
    # ---- evaluate the agent protocol with greedy decoding on fresh questions
    erng = np.random.default_rng(1234)
    tests, correct = [], 0
    for _ in range(a.eval_questions):
        ex = make_example(erng)
        q = ex.split('\n')[0][3:]; want_call = ex.split('\n')[1][3:]; want = ex.split('\n')[3][3:]
        call, ans = agent_episode(model, q)
        ok = (call == want_call and ans == want)
        correct += ok
        tests.append((q, want_call, want, ok))
    print('agent accuracy on %d fresh questions: %d/%d' % (a.eval_questions, correct, a.eval_questions))
    outputs = {}
    for q, want_call, want, ok in tests:
        outputs[q] = agent_episode(model, q)
    chosen = [t for t in tests if t[3]][:a.prompts]
    if len(chosen) < a.prompts:
        if not a.export_anyway:
            sys.exit('fewer than %d correct episodes; train longer' % a.prompts)
        chosen = tests[:a.prompts]
    # ---- export
    os.makedirs(a.out, exist_ok=True)
    order = ['wte', 'wpe'] + [k % l for l in range(a.layers) for k in ('ln1_g%d', 'ln1_b%d', 'wqkv%d', 'bqkv%d', 'wo%d', 'bo%d', 'ln2_g%d', 'ln2_b%d', 'w1%d', 'b1%d', 'w2%d', 'b2%d')] + ['lnf_g', 'lnf_b', 'head']
    with open(os.path.join(a.out, 'model_weights.h'), 'w') as f:
        f.write('/* Generated by tools/llm/train_agent_gpt.py -- do not edit.\n * Tiny tool-using GPT: d=%d, %d layers, %d heads, context %d, vocabulary %d.\n * %d parameters, float32, row-major as agent.c reads them. */\n'
                % (a.d, a.layers, a.heads, a.ctx, len(VOCAB), nparam))
        f.write('#define GPT_D %d\n#define GPT_L %d\n#define GPT_H %d\n#define GPT_T %d\n#define GPT_V %d\n' % (a.d, a.layers, a.heads, a.ctx, len(VOCAB)))
        f.write('static const char GPT_VOCAB[GPT_V + 1] = "%s";\n' % ''.join(VOCAB).replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n'))
        offs, total = {}, 0
        for k in order:
            offs[k] = total; total += model.p[k].size
        for k in order:
            f.write('#define OFF_%s %d\n' % (k.upper(), offs[k]))
        f.write('#define GPT_NPARAM %d\n' % total)
        f.write('#define GEN_CALL %d\n#define GEN_ANS %d\n' % (a.gen, a.gen))   # the generation lengths the numpy references used
        f.write('/* matmul weights (wqkv, wo, w1, w2, head) are stored transposed, [out][in], so each\n   output is one contiguous dot product; embeddings stay [row][d]. */\n')
        if a.blob:
            # a large model is not C text: the weights go to a raw float32 file that
            # agent.c embeds at link time (.incbin), found through the include path
            f.write('#define GPT_W_BLOB "model_weights.bin"   /* float32, the same order; agent.c .incbin\'s it */\n')
            with open(os.path.join(a.out, 'model_weights.bin'), 'wb') as fb:
                for k in order:
                    mat = model.p[k]
                    if mat.ndim == 2 and k not in ('wte', 'wpe'):
                        mat = mat.T
                    fb.write(np.ascontiguousarray(mat).astype(np.float32).ravel().tobytes())
        else:
            f.write('static const float GPT_W[GPT_NPARAM] = {\n')
            for k in order:
                mat = model.p[k]
                if mat.ndim == 2 and k not in ('wte', 'wpe'):
                    mat = mat.T
                arr = np.ascontiguousarray(mat).astype(np.float32).ravel()
                f.write('/* %s %s%s */\n' % (k, mat.shape, ' (transposed)' if mat is not model.p[k] else ''))
                for i in range(0, arr.size, 8):
                    f.write(','.join('%.9g' % x for x in arr[i:i + 8]) + ',\n')
            f.write('};\n')
    with open(os.path.join(a.out, 'prompts.h'), 'w') as f:
        f.write('/* Generated by tools/llm/train_agent_gpt.py: questions the numpy model answers\n * correctly with greedy decoding, with the tool call and answer expected. */\n')
        f.write('#define N_PROMPTS %d\n' % len(chosen))
        f.write('static const char *const PROMPT_Q[N_PROMPTS] = { %s };\n' % ', '.join('"%s"' % t[0] for t in chosen))
        f.write('static const char *const PROMPT_CALL[N_PROMPTS] = { %s };\n' % ', '.join('"%s"' % t[1] for t in chosen))
        f.write('/* the numpy model\'s own greedy outputs for these prompts (what agent.c must reproduce) */\n')
        f.write('static const char *const PROMPT_CALL_MODEL[N_PROMPTS] = { %s };\n' % ', '.join('"%s"' % outputs[t[0]][0] for t in chosen))
        f.write('static const char *const PROMPT_ANS_MODEL[N_PROMPTS] = { %s };\n' % ', '.join('"%s"' % outputs[t[0]][1] for t in chosen))
        f.write('static const char *const PROMPT_ANS[N_PROMPTS] = { %s };\n' % ', '.join('"%s"' % t[2] for t in chosen))
    print('wrote %s/model_weights.h (%d floats%s) and prompts.h (%d prompts)' % (a.out, total, ', as model_weights.bin' if a.blob else '', len(chosen)))
    for t in chosen[:3]:
        print('  e.g. Q: %s -> T: %s -> A: %s' % t[:3])

if __name__ == '__main__':
    main()
