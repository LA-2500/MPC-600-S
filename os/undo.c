// Undo and Redo (Shift + - and Shift + +), two steps each way. Before an audio command
// (Discard, Convert, a take assigned, pads copied ...) or a change to the sequences
// (recording, Erase, Timing Correct), a snapshot is kept of everything it could touch:
// every program's pads, the sounds kept in memory and every sequence. Sounds are not
// copied: a snapshot holds a reference, so a sound stays alive while it could come back.
#include "os.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct snap_pad {
    short program, slot;
    struct os_pad pad; // its onsets copied, its sound referenced
};

struct snapshot {
    char label[24];
    struct snap_pad *pads;
    int npads;
    struct {
        struct sample *s;
        char name[40];
    } memory[OS_MEMORY];
    int nmemory;
    char names[OS_PROGRAMS][17];
    int s600[OS_PROGRAMS];
    struct seq_sequence *seqs; // SEQ_COUNT, their notes and moves copied
};

static int32_t *copy_onsets(const struct os_pad *p) {
    int32_t *o = malloc((size_t)(p->nonsets > 0 ? p->nonsets : 1) * sizeof *o);
    if (o && p->nonsets > 0) memcpy(o, p->onsets, (size_t)p->nonsets * sizeof *o);
    return o;
}

static struct snapshot *take(struct os *s, const char *label) {
    struct snapshot *n = calloc(1, sizeof *n);
    if (!n) return 0;
    snprintf(n->label, sizeof n->label, "%s", label);
    int count = 0;
    for (int p = 0; p < OS_PROGRAMS; p++)
        for (int i = 0; i < OS_PADS; i++) count += s->programs[p].pads[i].s != 0;
    n->pads = malloc((size_t)(count ? count : 1) * sizeof *n->pads);
    n->seqs = malloc(sizeof(struct seq_sequence) * SEQ_COUNT);
    if (!n->pads || !n->seqs) {
        free(n->pads);
        free(n->seqs);
        free(n);
        return 0;
    }
    for (int p = 0; p < OS_PROGRAMS; p++) {
        memcpy(n->names[p], s->programs[p].name, sizeof n->names[p]);
        n->s600[p] = s->programs[p].s600;
        for (int i = 0; i < OS_PADS; i++) {
            const struct os_pad *pad = &s->programs[p].pads[i];
            if (!pad->s) continue;
            struct snap_pad *k = &n->pads[n->npads++];
            k->program = (short)p, k->slot = (short)i, k->pad = *pad;
            k->pad.onsets = copy_onsets(pad);
            if (!k->pad.onsets) k->pad.nonsets = 0;
            pad->s->refs++;
        }
    }
    for (int m = 0; m < s->nmemory; m++) {
        n->memory[m].s = s->memory[m].s;
        memcpy(n->memory[m].name, s->memory[m].name, sizeof n->memory[m].name);
        n->memory[m].s->refs++;
    }
    n->nmemory = s->nmemory;
    pthread_mutex_lock(&s->seq.lock);
    memcpy(n->seqs, s->seq.sequence, sizeof(struct seq_sequence) * SEQ_COUNT);
    for (int q = 0; q < SEQ_COUNT; q++)
        for (int t = 0; t < SEQ_TRACKS; t++) {
            struct seq_track *k = &n->seqs[q].track[t];
            const struct seq_track *o = &s->seq.sequence[q].track[t];
            k->notes = 0, k->autos = 0, k->cap = k->count, k->capauto = k->nauto;
            if (o->count) {
                k->notes = malloc((size_t)o->count * sizeof *k->notes);
                if (k->notes) memcpy(k->notes, o->notes, (size_t)o->count * sizeof *k->notes);
                else k->count = k->cap = 0;
            }
            if (o->nauto) {
                k->autos = malloc((size_t)o->nauto * sizeof *k->autos);
                if (k->autos) memcpy(k->autos, o->autos, (size_t)o->nauto * sizeof *k->autos);
                else k->nauto = k->capauto = 0;
            }
        }
    pthread_mutex_unlock(&s->seq.lock);
    return n;
}

static void free_seqs(struct seq_sequence *seqs) {
    for (int q = 0; q < SEQ_COUNT; q++)
        for (int t = 0; t < SEQ_TRACKS; t++) {
            free(seqs[q].track[t].notes);
            free(seqs[q].track[t].autos);
        }
    free(seqs);
}

// Lets a snapshot go: its references, its copies.
static void drop(struct os *s, struct snapshot *n) {
    if (!n) return;
    for (int i = 0; i < n->npads; i++) {
        free(n->pads[i].pad.onsets);
        os_sample_release(s, n->pads[i].pad.s);
    }
    for (int m = 0; m < n->nmemory; m++) os_sample_release(s, n->memory[m].s);
    free(n->pads);
    free_seqs(n->seqs);
    free(n);
}

// Puts a snapshot back, taking over what it holds. Only pads that differ reach the engine.
static void apply(struct os *s, struct snapshot *n) {
    static struct os_pad was[OS_PADS];
    for (int p = 0; p < OS_PROGRAMS; p++) {
        struct os_pad *pads = s->programs[p].pads;
        memcpy(was, pads, sizeof was);
        for (int i = 0; i < OS_PADS; i++) {
            if (pads[i].s) free(pads[i].onsets);
            memset(&pads[i], 0, sizeof pads[i]);
        }
        for (int k = 0; k < n->npads; k++)
            if (n->pads[k].program == p) pads[n->pads[k].slot] = n->pads[k].pad; // its reference moves with it
        memcpy(s->programs[p].name, n->names[p], sizeof n->names[p]);
        os_program_set_s600(s, p, n->s600[p]);
        for (int i = 0; i < OS_PADS; i++) {
            int same = was[i].s == pads[i].s && !memcmp(&was[i].p, &pads[i].p, sizeof pads[i].p) &&
                       !memcmp(&was[i].sp, &pads[i].sp, sizeof pads[i].sp);
            if (!same) os_slot_refresh(s, p, i);
        }
        for (int i = 0; i < OS_PADS; i++) os_sample_release(s, was[i].s); // after: a sound in both stays
    }
    struct sample *old[OS_MEMORY];
    int nold = s->nmemory;
    for (int m = 0; m < nold; m++) old[m] = s->memory[m].s;
    for (int m = 0; m < n->nmemory; m++) {
        s->memory[m].s = n->memory[m].s;
        memcpy(s->memory[m].name, n->memory[m].name, sizeof s->memory[m].name);
    }
    s->nmemory = n->nmemory;
    for (int m = 0; m < nold; m++) os_sample_release(s, old[m]);

    pthread_mutex_lock(&s->seq.lock);
    struct seq_sequence *gone = malloc(sizeof(struct seq_sequence) * SEQ_COUNT);
    if (gone) memcpy(gone, s->seq.sequence, sizeof(struct seq_sequence) * SEQ_COUNT);
    memcpy(s->seq.sequence, n->seqs, sizeof(struct seq_sequence) * SEQ_COUNT);
    for (int i = 0; i < 256; i++) s->seq.rec[i].open = 0;
    pthread_mutex_unlock(&s->seq.lock);
    if (gone) free_seqs(gone);
    free(n->seqs);
    free(n->pads);
    free(n);
    s->erase_ask = 0;
    s->eq_seq = -1; // the track EQs may have changed: send them again
}

static void push(struct os *s, struct snapshot **stack, int *count, struct snapshot *n) {
    if (*count == OS_UNDO_STEPS) { // the oldest goes
        drop(s, stack[0]);
        memmove(stack, stack + 1, (OS_UNDO_STEPS - 1) * sizeof *stack);
        (*count)--;
    }
    stack[(*count)++] = n;
}

void os_undo_save(struct os *s, const char *label) {
    struct snapshot *n = take(s, label);
    if (!n) return;
    push(s, s->undo, &s->nundo, n);
    while (s->nredo > 0) drop(s, s->redo[--s->nredo]); // a new change: what was undone is gone
}

static int recording(struct os *s) {
    pthread_mutex_lock(&s->seq.lock);
    int r = s->seq.mode == SEQ_RECORDING || s->seq.mode == SEQ_OVERDUB || s->seq.mode == SEQ_COUNT_IN;
    pthread_mutex_unlock(&s->seq.lock);
    return r;
}

static void step(struct os *s, struct ui *u, int redo) {
    struct snapshot **from = redo ? s->redo : s->undo, **to = redo ? s->undo : s->redo;
    int *nfrom = redo ? &s->nredo : &s->nundo, *nto = redo ? &s->nundo : &s->nredo;
    char msg[40];
    if (*nfrom == 0) {
        os_toast(s, u, redo ? "Nothing to redo" : "Nothing to undo");
        return;
    }
    if (recording(s)) {
        os_toast(s, u, "Stop recording first");
        return;
    }
    struct snapshot *n = from[--*nfrom];
    struct snapshot *now = take(s, n->label); // so it can be stepped back again
    snprintf(msg, sizeof msg, "%s: %s", redo ? "Redo" : "Undo", n->label);
    apply(s, n);
    if (now) {
        struct snapshot **stack = to;
        if (*nto == OS_UNDO_STEPS) {
            drop(s, stack[0]);
            memmove(stack, stack + 1, (OS_UNDO_STEPS - 1) * sizeof *stack);
            (*nto)--;
        }
        stack[(*nto)++] = now;
    }
    os_toast(s, u, msg);
}

void os_undo(struct os *s, struct ui *u) {
    step(s, u, 0);
}

void os_redo(struct os *s, struct ui *u) {
    step(s, u, 1);
}

// Every undo and redo step let go (a project loaded: the old one's steps no longer apply).
void os_undo_clear(struct os *s) {
    for (int i = 0; i < s->nundo; i++) drop(s, s->undo[i]);
    for (int i = 0; i < s->nredo; i++) drop(s, s->redo[i]);
    s->nundo = s->nredo = 0;
}
