// Help: Shift + SAMPLE REC opens the current page's help at its first page, Shift + SEQ REC
// (Recall) at its second. Where a page's help runs longer, B1 and B3 turn the pages. The main
// sequence page has none: there the two move through the sequence a bar at a time.
//
// The helper stands beside the text: tapping her foot, then pointing her stick at it.
#include "lcd.h"
#include "os.h"
#include "helper_sprite.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TEXT_COLS 20  // the text's width; the helper takes the right of the screen
#define TEXT_ROWS 11  // rows 3 to 13
#define SMALL_COLS 40 // the half-size font, for a page that will not fit: 40 across,
#define SMALL_ROWS 16 // 16 down (10 pixels a line)
#define SMALL_Y (3 * CELL_H + 2)
#define MAX_PAGES 24
#define PAGE_LINES 16

// Each page's help: paragraphs, wrapped to fit; "\f" starts a new help page.
static const struct {
    int screen, record_page; // record_page: for OS_RECORD only, else -1
    const char *title;
    const char *text;
} HELP[] = {
    {OS_SAMPLE, -1, "Edit Sound",
     "Edits the pad last pressed. K1-K3 set the three values shown; the slider moves the "
     "framed one.\n"
     "Hold SAMPLE and press a pad: selected without playing it (a loop keeps going as it is).\n"
     "Pages:\nB1: Trim, Vol.\nB2: Tune, Poly, Envel, Vel Mod.\nB3: Filter, Flt Env.\nPress a B key again "
     "for the next page in its group.\f"
     "Hidden keys:\n"
     "Shift+B1 Loop Lock.\nShift+B2 Normalize.\nShift+B3 Edit All Pads (an edit goes to every "
     "pad).\n"
     "Hold SAMPLE: B1/B2 zoom the wave.\n"
     "Shift+CHOP Note On. Shift+LOOP Reverse.\n"
     "ERASE on Sample: rename the sound.\f"
     "Trim page: K3 picks a Do action (Discard, Extract, Silence, Reverse, Copy, Convert...). "
     "B3 then does it.\n"
     "On Trim the slider steps Zone Start through the hits it found.\n"
     "Shift+Pad 13 opens Trim from anywhere.\f"
     "Tips: Sample: at the top swaps the pad's sound for any sound in memory.\n"
     "Shift + - undoes a Discard or other audio command; Shift + + redoes it."},
    {OS_LOOP, -1, "Loop",
     "Loop on or off, and its points: K1 Loop St, K2 Loop End, K3 picks a Do.\n"
     "Find St and Find End search for where the loop joins most smoothly. X-Fade blends the "
     "join. Discard keeps Zone Start to Loop End.\f"
     "Tips: hold SAMPLE and use B1/B2 to zoom on the join.\n"
     "Shift+LOOP turns Reverse on and off for the pad."},
    {OS_S600, -1, "S600",
     "An S600 program plays each pad as a two-oscillator synth: its sound and Layer 2, through "
     "envelopes, filter and LFO.\n"
     "Pages:\nB1: Env1, Env2, Trim.\nB2: Filter, LFO, Mod.\nB3: Main, Layer (S600 MULTI: Main, MULTI; "
     "its velocity layers are its keygroups' zones).\f"
     "Root Note (Main): the key the sound plays at its own pitch. MIDI keys play the Instrument "
     "(Program Config) from there. Auto-Detect ON (Root Auto-Detect, Additional Settings 2) finds "
     "it when a sound is loaded or recorded: from the note in its name and the pitch heard, with "
     "Fine set to put it in tune. None found: C4. The pad itself always plays it at its own pitch.\f"
     "Mod: what the wheels do.\n"
     "Mod Wheel, K1 Pitch: vibrato (30 to start). K2 Filter: the wheel on the filter, by Filter "
     "Mode: LFO adds the LFO's movement; OPEN opens the cutoff, up to three octaves. K3 Level: "
     "tremolo from the LFO. All at the LFO page's Rate and Shape.\n"
     "Pitch Bend: Bend Up and Bend Down, in semitones.\f"
     "Hidden keys:\n"
     "Shift+B3 Edit All Pads.\n"
     "Shift+CHOP Note On.\n"
     "Filter: S600 LPF is the Moog-style ladder: thick, resonant, it sings at the top of Reson.\f"
     "Main: Volume, Semi, Fine on K1-K3; Poly or Mono, Legato (mono notes slide without "
     "restarting), Portamento, Note On, Root Note.\n"
     "Layer: a second sound through the same envelopes and filter.\f"
     "Tips: Templates on Env1/Env2 give a starting shape. All On (the default): full level from "
     "the start of the note until it is let go, the sound as it is. Then MPC Bass, Piano, Brass ...\n"
     "S600 waveforms: SINE, SAW, SQUARE, PULSE, single cycles in MPC600S/s600 on the card. "
     "Load one onto a pad with Save/Load, type S600 OSC, then turn Loop on (LOOP) so it "
     "plays as a held note.\f"
     "MIDI keys play the selected pad chromatically."},
    {OS_LEVELS, -1, "16 Levels",
     "One sound across the 16 pads: by Velocity (soft to hard) or Tune (a scale of pitches).\n"
     "Press a pad to pick the sound (Velocity) or the root (Tune).\f"
     "LEVELS cycles Velocity, Tune, then off.\n"
     "Tip: in Tune, PAD BANK reaches further up and down the scale."},
    {OS_MIXER, -1, "Track Mixer",
     "Sixteen tracks a bank; PAD BANK for the next. A pad picks its track's strip.\n"
     "Slider: level (it takes over once it reaches the value). Wheel: pan.\n"
     "B1 Mute.\nB2 Solo.\nB3 EQ.\f"
     "Hidden keys:\n"
     "Shift+B1 Reset EQ.\n"
     "Shift+B2 MIDI CC on/off.\n"
     "Shift+B3 FX Sends: each track's sends to the Track/Mixer effects. There, Shift+B3 again: "
     "the channel's Insert Effects.\n"
     "Tip: the mixer answers MIDI CC: set it up on MIDI Settings (Shift+Pad 8)."},
    {OS_EQ, -1, "EQ Settings",
     "The track's three-band EQ. Type 1073 (stepped) or 4000 E (any frequency, mid Q).\n"
     "K1-K3 set the gains. EQ: OFF is a true bypass, as is flat.\n"
     "B1 goes back to the mixer."},
    {OS_RECORD, 0, "Sample New Sound",
     "Records the input. Set the Input, Mode, Pitch, Thres(hold), Bit Rate, Sample Rate, "
     "Anti-Aliasing and Normalize, then <Rec>.\n"
     "Input: LINE (the jack), INST L (a guitar or bass in the left input, mono), MIC (built "
     "in) or USB (a computer's audio, over USB).\n"
     "It starts when the level passes Thres.\f"
     "When done: press a pad to assign it, ENTER to keep it in memory, <Play> to hear it.\n"
     "Pitch speeds the take up; the pad is tuned down to match, the way old samplers saved "
     "time.\n"
     "SAMPLE REC again: Audiotrack Record."},
    {OS_RECORD, 1, "Audiotrack Record",
     "A take in time with the sequence: <Rec> plays it from the start after the count-in, "
     "and the take begins exactly on its first beat.\n"
     "Length: the sequence, twice it, or until STOP.\f"
     "Press an empty pad: it gets the take and a note at the start of the active track.\n"
     "Pitch works as on Sample New Sound.\n"
     "<Live Loop>: takes that drop in while the sequence keeps looping.\n"
     "SAMPLE REC again: Convert Sample."},
    {OS_RECORD, 6, "Auto Sampler",
     "Records an instrument note by note and makes it the S600 MULTI program's Instrument: you "
     "play each sample asked for, and at the end every sample is mapped by its pitch and "
     "velocity layer. (Type S600 MULTI in Program Config first.)\f"
     "Before recording:\n"
     "In, Mode: the input, and stereo or mono (MONO LR to start). INST L records mono.\n"
     "Threshold: the level that starts each take (-30 dB to start); OFF starts each at once.\n"
     "Samples: how many notes you will play (up to 32).\n"
     "Layers: 1, or 2: a soft and a loud take of each note.\f"
     "Length: how long each take records from the threshold (3.0 s to start), or MANUAL: until "
     "you press ENTER.\n"
     "Rate, Bits, AA: the sound's rate (29.76 kHz), bits (12) and anti-aliasing, as on Sample "
     "New Sound.\n"
     "Normalize: ALL (to start) raises every take by the same amount, so soft takes stay "
     "softer; EACH brings each to full level; OFF leaves them as played.\n"
     "Key Tracking: the Key Track template the Instrument gets (Flat, Piano, Strings ...): its "
     "envelopes, filter and KEY>PAN, KEY>FILTER, KEY>LEVEL, KEY>ENV, as on the MULTI Key Track page.\f"
     "B3 <Rec> starts. If the program already has sounds it asks: Clear all existing samples "
     "in program?\n"
     "Yes: they come off its pads (kept in memory).\n"
     "No: they stay; the takes go on the empty pads.\n"
     "Cancel: nothing starts.\f"
     "Recording:\n"
     "Ready for Sample 1, Velocity 1: play the note. It records from the threshold for the "
     "Length (MANUAL: until ENTER), then asks for the next at once: Sample 1, Velocity 2 (play "
     "it harder), then Sample 2 ... Play the notes in any order, a few semitones apart for a "
     "natural instrument, an octave apart for a more stretched, lo-fi one.\f"
     "While recording:\n"
     "B2 <Re-Do>: the take again (while waiting, the last one).\n"
     "B3 <Skip>: leave out the take asked for; the keygroups either side cover its keys.\n"
     "B1 <End>: stop here. Map current recorded samples? Yes maps what there is; No lets the "
     "whole recording go.\f"
     "Multi Sample Recording Complete: each sample's note is found from its pitch. One with no "
     "clear pitch is asked about: turn the wheel or play a key for its note, B2 <Play> to hear "
     "it, B3 <OK>; or B1 <Leave Out> (it stays on its pad, not in the keygroups).\f"
     "Then the takes go onto pads in pitch order from the Instrument, each note's soft and "
     "loud takes side by side, named for their notes (AS C3, AS C3 v2), and the keygroups are "
     "made: one a note, Velocity 1 its Zone 1, Velocity 2 its Zone 2 (from velocity 65), the "
     "ranges midway between, the ends to the ends of the keyboard. The Keygroup page shows it."},
    {OS_RECORD, 5, "Live Looper",
     "Select an empty pad, then <Rec>: one pass of the sequence is recorded and the sequence "
     "keeps looping.\n"
     "Stopped: a count-in first. Playing: the take starts on the next bar 1.\f"
     "The take drops onto the pad, with a note at the start of the active track, on the pass "
     "it is ready in.\n"
     "Select another empty pad and <Rec> again to add a layer, without stopping.\f"
     "<Del Take>: the last take's sound and note go, and its pad is selected to try again. "
     "Pressed again, the take before.\n"
     "<Stop> lets a take go. <Back>: Audiotrack Record.\n"
     "SAMPLE + pad: select a loop to edit without retriggering it.\n"
     "Footswitch AUDIO REC (MIDI Settings): the pedal is <Rec>/<Stop>, here or on any page "
     "once takes have started.\n"
     "How a take is converted: Loop Conv on Additional Settings."},
    {OS_RECORD, 2, "Convert Sample",
     "The selected pad's sound sampled again at a new rate, bits and pitch, as an old sampler "
     "would. It replaces the sound on the pad; the original stays in memory.\n"
     "SAMPLE REC again: Stems."},
    {OS_RECORD, 3, "Stems",
     "Splits a pad's sound into drums, bass, vocals or music.\n"
     "EXTRACT keeps only that stem; REMOVE keeps all but it.\n"
     "Up to 20 seconds.\f"
     "Tip: for a longer loop, Convert it with Pitch +12 first: half the length, then split.\n"
     "SAMPLE REC again: back to Sample New Sound."},
    {OS_RECORD, 4, "Resample",
     "Records the main output: live as you play, or the whole sequence from its start.\n"
     "Start: THRESHOLD waits for the sound to reach Thres; MANUAL starts at once on <Rec>.\n"
     "Input Thru: MUTE keeps the input out of the take, RECORD puts it in.\n"
     "Opened with Shift+Pad 11."},
    {OS_REPEAT, -1, "Note Repeat",
     "Hold NOTE REPEAT, then hold pads: they repeat at the Rate. Press harder for louder.\n"
     "1/8, 1/16 and 1/32 take the swing.\n"
     "B2 <Rate> steps the rate; the slider runs through them too.\n"
     "K1 the rate. Arpeggiator: K2 the category, K3 the pattern.\f"
     "B Keys, with NOTE REPEAT held:\n"
     "HOLD: a B key switches the rate while held.\n"
     "TOGGLE: pressed, it stays until pressed again.\f"
     "Type ARPEGGIATOR: the held pads play one at a time in a Pattern, chosen by Category.\n"
     "Chord patterns go through the held pads, lowest first; riffs play their tune from the "
     "lowest pad held; Chord rhythms play the held chord in a groove.\n"
     "Categories include JAZZ lines and BASSLINES (pop, funk, disco, R&B, rap).\f"
     "Octaves: chord patterns climb up through 1 to 4 octaves.\n"
     "Gate: how long each note sounds.\n"
     "Latch: the notes go on after you let go, until a new chord.\n"
     "Trans: everything up or down a semitone at a time.\f"
     "STEP and USER are step patterns: notes with accents (louder), slides (the note glides "
     "into the next) and ties.\n"
     "B3 <Edit>: the Pattern Edit page, from anywhere here. On a STEP pattern it is copied "
     "into an empty USER one first; otherwise you get the USER pattern edited last.\f"
     "Tip: hold pads in 16 Levels Tune to play a riff or chord pattern on any one sound.\n"
     "Tip: an S600 bass with the S600 LPF and the Bass or Synth riffs."},
    {OS_ZONES, -1, "Zone Assign",
     "Divides a sound into zones, each on a separate pad.\n"
     "PAD: play the sound and press pads where each zone starts. REGIONS: equal zones, "
     "1 to 32.\n"
     "TRANSIENT: a zone at every drum hit, found at once. Made for drum breaks: kicks, snares, "
     "hats and ghost notes, each zone starting just before its hit.\f"
     "TRANSIENT:\n"
     "Sensitivity 1-10 (5 to start): higher finds softer hits (ghost notes, quiet hats); lower "
     "keeps only the main hits.\n"
     "Zone, Start: move a zone's start with the wheel, a millisecond a click.\n"
     "B2 <Delete>: take a false zone out; the one before reaches over its part.\f"
     "Snap moves each mark to the nearest hit.\n"
     "Then assign them to a new program or to empty banks; the zones play to their ends "
     "(Note Off)."},
    {OS_MULTI, 0, "MULTI: Keygroup",
     "An S600 MULTI program is a multisample instrument: one pad, the Instrument, plays samples "
     "from the program's pads across the keyboard. (An S600 program is a two-oscillator synth: "
     "a sound and its Layer 2.)\n"
     "Set it up in Program Config: Type S600 MULTI, Inst the Instrument pad (A01 to start).\f"
     "A keygroup is a range of keys playing one sample:\n"
     "Lo, Hi: its lowest and highest key.\n"
     "Root: the key the sample plays at its own pitch; the keys above and below play it higher "
     "and lower.\n"
     "Zone 1: the pad whose sample it plays (with that pad's trim and loop).\f"
     "Two velocity layers:\n"
     "Zone 2: a second pad, played by harder notes.\n"
     "Zone 2 From Velocity: the softest velocity that plays Zone 2 (65 to start). Softer notes "
     "play Zone 1. Shown only with a Zone 2 set.\n"
     "No Zone 2: Zone 1 plays at every velocity.\f"
     "KG: the keygroup shown, and how many. K1 moves through them; so does playing a key.\n"
     "Ranges AUTO: Lo and Hi always reach midway to the roots either side, so moving a root "
     "or adding a sample spreads the keys again. Setting Lo or Hi by hand makes it MANUAL.\f"
     "Tune, Fine: the keygroup's tuning (semitones, cents). Fine is set for you when a pitch "
     "is found, so the sample plays in tune.\n"
     "Level, Pan: the keygroup's own, on top of the Instrument's.\f"
     "Quick ways in:\n"
     "Cursor on Root, Lo or Hi, play a key: it is set to that key.\n"
     "Cursor on Zone 1 or Zone 2, press a pad: that pad is the zone.\n"
     "Pads pressed here play their own sound; the screen stays on the Instrument.\f"
     "Samples loaded by hand:\n"
     "Press a pad, then SHIFT + B3 <Add Pad>: its root is found from its name and pitch (C4 if "
     "neither tells) and a keygroup is made for it, the ranges spread again. A root already "
     "there with no Zone 2 takes it as its Zone 2.\n"
     "SHIFT + B2 <Remove>: the keygroup shown goes; the rest spread to cover it.\f"
     "The keyboard: three octaves at a time, following the keygroup shown. Each keygroup is a "
     "bar over its keys, the one shown solid; each root is marked on its key; each C is named.\f"
     "B1 <KeyGrp>/<KeyTrk>: this page and Key Track.\n"
     "B2 <Smplr>: the Auto Sampler, to record an instrument note by note.\n"
     "B3 <AutoMap>: build every keygroup from the program's pads.\n"
     "SAMPLE: back to the S600 pages (Main): envelopes, filter and LFO are the Instrument's, "
     "shared by every keygroup."},
    {OS_MULTI, 1, "MULTI: Key Track",
     "Key tracking changes the sound with the key played, around C4 (middle C), as real "
     "instruments do across their range.\f"
     "KEY>PAN (-50 to +50): spreads the keys across the stereo field. + puts low notes left and "
     "high notes right (a player's view of a piano); - the other way. The full amount is "
     "reached two octaves from C4.\f"
     "KEY>FILTER (0 to 100): the cutoff follows the key. At 100 it moves an octave for each "
     "octave played, so high notes stay as bright as low ones and stretched samples do not "
     "dull. It only acts when the filter is not fully open (Cutoff on the Filter page).\f"
     "KEY>LEVEL (-100 to +100): the level changes with the key, 6 dB an octave at 100. - makes "
     "high notes softer, as strings and hammers ring.\f"
     "KEY>ENV (0 to 100): decays and releases change with the key. At 100 they halve an octave "
     "up and double an octave down: high notes ring shorter, low notes longer, like a piano.\f"
     "Template: sets up the Instrument for a kind of instrument in one go: Env1 and Env2, the "
     "filter, Poly or Mono, and the four KEY> values.\n"
     "Flat (all off), Piano, Elec. Piano, Organ, Strings, Choir, Brass, Woodwind, Guitar, "
     "Bass, Synth Pad, Synth Lead (mono, legato), Mallets, Bells.\f"
     "A * after the template's name: something has been changed since it was set. Everything "
     "stays editable here and on the S600 pages.\n"
     "Starting points: keys, Piano or Elec. Piano; pads and strings, Strings or Synth Pad; "
     "basses, Bass; leads, Synth Lead."},
    {OS_MULTI, 2, "MULTI: Auto Map",
     "Builds every keygroup from the program's pads in one go: a keygroup for each sample, at "
     "its root, the ranges spread between them. The keygroups the Instrument had are replaced.\f"
     "Root From:\n"
     "NAME+PITCH (to start): the note in the sample's name (Piano C3, pno_F#2, EP-Bb1), in the "
     "octave nearest the pitch heard (sample sets number their octaves either way).\n"
     "NAME: the name alone (C4 is middle C).\n"
     "PITCH: the pitch heard alone.\n"
     "The pitch heard also sets Fine, so each sample plays in tune.\f"
     "Span: MIDWAY (to start) reaches midway to the roots either side. ROOT UP: from its root "
     "up to the next. ROOT DOWN: from the one before up to its root.\n"
     "Velocity: 1 ZONE, or 2 ZONES: two samples on one root become its soft and loud zones, the "
     "quieter one Zone 1.\f"
     "Extend Last Note: ON (to start) stretches the lowest and highest keygroups to the ends of "
     "the keyboard, so every key plays; OFF stops them at their roots.\n"
     "Samples with no note found are left out (the message says how many); add them on the "
     "Keygroup page.\n"
     "B3 <Do It> builds it; B1 <Cancel>. Undo puts the old keygroups back."},
    {OS_PROGRAM, -1, "Program Config",
     "The pads of a program: what sound is on each.\n"
     "Type: MPC (drums), S600 (each pad a two-oscillator synth: a sound and its Layer 2), or "
     "S600 MULTI (a multisample instrument: one pad plays samples from the others across the "
     "keyboard).\n"
     "Inst (S600 and S600 MULTI): the Instrument, the pad MIDI keys play. A01 to start; turn "
     "the wheel, or with the cursor here press a pad. LAST PAD (S600): the pad pressed last.\f"
     "Copy, Swap, Delete work on single pads: choose, then press the other pad.\f"
     "Hidden keys: <Copy Pgm>, <Clear Pgm> with Shift.\n"
     "Snd: gives the selected pad any sound in memory."},
    {OS_TC, -1, "Timing Correct",
     "Note value and Swing for recording, Shift timing to push notes early or late.\n"
     "Move Existing: corrects notes already recorded over the range; press pads to pick which.\n"
     "K1 Swing, K2 Shift amount, K3 the Move Existing track.\f"
     "Shift+Pad 10 turns Timing Correct off and back on.\n"
     "Shift+Pad 14 opens this page."},
    {OS_METRO, -1, "Metronome",
     "Click on or off, In Play (also while playing), volume and rate.\n"
     "Audio Rec: clicks during audio track and Live Looper takes (NO by default).\n"
     "After Loop: CLICK goes on after a Live Looper take, STOP keeps quiet.\n"
     "Count-in: bars before recording, or wait for a pad or key to start."},
    {OS_ERASE, -1, "Erase",
     "Erases notes from a sequence: a track or all, a range, and pads you press (none is "
     "all).\f"
     "While playing, hold ERASE and pads: those notes go as the sequence passes them.\n"
     "K1 the sequence, K2 the track."},
    {OS_SEQCOPY, -1, "Copy Sequence",
     "Copies a whole sequence over another: its tracks, notes, fader moves, bars, tempo and "
     "name.\n"
     "From: the sequence copied. To: where it goes; a pad picks it (A01 is Seq 1, B01 Seq 17).\n"
     "A sequence with notes is replaced only after you say so. Shift+- undoes.\f"
     "Shift+B3 on the main page opens this page (hold SHIFT to see <Copy>), and so does "
     "B1 on Copy Events."},
    {OS_COPYEV, -1, "Copy Events",
     "Copies notes and fader moves: from a sequence and track (0 is every track, each onto "
     "its own), a range and pads you press (none is all), to a sequence, track and start.\n"
     "MERGE adds them; REPLACE clears that span first. Copies: end to end. Past the end of "
     "the sequence is left out.\f"
     "K1 the From sequence, the slider the From track, K2 the To track, K3 the end (by bars).\n"
     "B1 Copy Sequence, B2 Copy Bars, B3 Do It. SHIFT+B1 or SHIFT+ERASE goes back."},
    {OS_COPYBARS, -1, "Copy Bars",
     "B2 <Mode>: COPY bars of one sequence in before a bar of another (the bars after move "
     "on), INSERT BLANK bars, or DELETE bars (the bars after move back).\n"
     "Before bar past the last puts them at the end. 999 bars at most; a sequence keeps one.\f"
     "B3 does it; Shift+- undoes. B1 goes back to Copy Events."},
    {OS_ARPEDIT, -1, "Pattern Edit",
     "A USER step pattern, played from the lowest pad held.\n"
     "Pads 1-12 enter C to B into the selected step and move on.\n"
     "Pads 13 and 14 put it an octave down or up, 15 accent, 16 slide: set them first, then "
     "the note.\f"
     "B2 <Rest>, B3 <Tie>: enter a rest, or carry the note before on.\n"
     "Accent: full velocity (others 70%). Slide: the note holds and glides into the next.\n"
     "Len: 1 to 16 steps.\f"
     "K1 moves the selected step. K2 turned right puts an accent on it, left takes it off; "
     "K3 the same for a slide. They work from where they are.\f"
     "Shift+B2 <Play>: the pattern on the selected pad, while you edit.\n"
     "Shift+B1 <Clear>: all steps back to C.\n"
     "ENTER and the wheel edit the fields. Kept on the card."},
    {OS_MIDI, -1, "MIDI Settings",
     "Ports (Ext, USB), Base and Receive channels, Sync send or receive, Local Mode, Soft "
     "Thru, Sustain pedal.\n"
     "Sustain FOOTSWITCH: the pedal is a Footswitch: PLAY/STOP, REC/OVERDUB, or AUDIO REC, "
     "which is <Rec> and <Stop> on Audiotrack Record and Live Looper.\f"
     "Foot Chn: ANY (to start) or 1-16. Foot CC: 64 (to start). Polarity: AUTO (to start) "
     "works out a pedal that rests 'down' from its first press; NORMAL or INVERTED.\n"
     "On those fields B2 is <Learn>: press it, then the pedal. Its channel, CC and polarity "
     "are set from that one press.\n"
     "The bottom line shows what the pedal last sent, to check it is getting through.\f"
     "<CC>: Mixer CC, the mixer under MIDI CC.\n"
     "<Notes>: the note each pad sends and answers to. <Learn> on either listens for one."},
    {OS_CCMAP, -1, "CC Map",
     "Mixer CC: ON, and the mixer follows MIDI CC.\n"
     "Target: BANK, channels 1-16 are the bank's 16 strips; SELECTED, the Base Chn moves the "
     "selected track.\n"
     "<Learn>, then move a knob or fader on your controller: the selected control takes its "
     "CC, Mixer CC goes on, and the next control is selected. <Learn> again for the next.\n"
     "Or set a CC with the wheel.\n"
     "<Default>: every CC back to OFF."},
    {OS_PADNOTES, -1, "Pad Notes",
     "The MIDI note each pad sends and answers to. Press a pad, then set its note, or "
     "<Learn> and play it.\n"
     "<Template>: every pad at once: DEFAULT MPC, CHROMATIC (a semitone a pad from C in the "
     "Octave), or SCALE (a scale from its Root and Octave; 35 scales). <Apply> sets them."},
    {OS_PRINT, -1, "Print Effects",
     "One effect printed onto a pad's sound: reverb on a snare, delay on a hat.\n"
     "Category, then Effect. K1-K3 its controls. Tail: AUTO or a time.\f"
     "<Play> hears it first. <Print> replaces the pad's sound; the original stays in memory "
     "and Shift + - undoes it.\n"
     "Tip: the EQ category has curves for kick, snare, hats, claps, toms and 808s."},
    {OS_INFX, -1, "Input Effects",
     "Nine effects, used three ways (Type). They start off and empty at power-on.\n"
     "INPUT: a chain on the input, heard and recorded.\n"
     "TRACK/MIXER: nine send effects for the mixer.\n"
     "PAD: a chain on one pad's sound; press a pad to move it.\f"
     "Each Type keeps its own chain: switch to INPUT to record, and back to TRACK/MIXER, and "
     "the mix is as it was.\n"
     "Category and Preset: guitar, bass, keyboard, other, mix.\n"
     "TRACK/MIXER starts on USER TEMPLATE: nine blank sends to make your own. Save it as "
     "TRACK FX on Save/Load.\f"
     "<Select> moves through the effects; Effect # sets one; K1-K3 its controls.\n"
     "INPUT and PAD: one reverb and one delay at most.\f"
     "Wah Pedal: the mod wheel, or the slider with the cursor on Pedal.\n"
     "Input: INST L for a guitar or bass straight into the left input: mono, with Gain "
     "(0-30 dB) and Brt (Bright: puts back the treble a passive pickup loses). A buffer pedal "
     "first sounds best. USB: a computer's audio.\n"
     "Sends: the mixer, Shift+B3."},
    {OS_SENDS, -1, "FX Sends",
     "Each track's send to each of the nine Track/Mixer effects. B1/B2 pick the effect; a "
     "pad picks the track; the slider or wheel sets the send.\f"
     "0 sends nothing at all. Reverbs and delays come back as effect only; compressors and "
     "drives come back whole, for parallel squash and grit.\n"
     "Works with KNOB FX Type TRACK/MIXER.\n"
     "SHIFT + B3 <Inserts>: the channel's Insert Effects. B3 alone: back to the mixer."},
    {OS_INSERT, -1, "Insert Effects",
     "Effects in a channel: the track's sound goes through them, one after another, and out. "
     "Nothing of the dry sound is added back: each effect's own Mix decides that.\n"
     "Order: the inserts, then the fader and pan, then the sends (they get the sound after "
     "the inserts), then the EQ.\f"
     "Getting here: SHIFT + Pad 9 (the mixer), a pad to pick the track, SHIFT + B3 (FX Sends), "
     "SHIFT + B3 again. B1 <Back>: FX Sends.\n"
     "B3 <Add>: an insert at the end of this track's chain (an EQ to start). B2 <Remove>: the "
     "one shown.\f"
     "Insert slots: there are 8, shared by every track. Each effect uses as many as it costs "
     "to run, shown after its name, e.g. ASR HALL (4):\n"
     "1 slot: EQs, compressors, the gate, most drives, the Juno chorus, flanger, wahs, the "
     "speaker.\n"
     "2 slots: amps, phasers, the Uni-Vibe, delays, the Vinyl Sim, the MFC42.\n"
     "4 slots: reverbs.\f"
     "So a reverb takes 4 of the 8 at once (Slots: 4/8 used): two reverbs fill them all; one "
     "reverb leaves room for an amp and two EQs. The bottom line shows what the one shown uses "
     "and what is left.\n"
     "An effect that does not fit is not taken: it says what it needs, e.g. ASR HALL needs 4 "
     "slots: 3 left. Remove one, or choose a lighter effect.\f"
     "Why slots: every insert runs all the time, live. The slots cap what the inserts can "
     "cost, whatever is chosen. (Many heavy voices and sends add load of their own.)\n"
     "CPU: how much of each audio block the MPC is taking now (voices, effects, everything). "
     "Keep it clear of 100%.\f"
     "Tip: use reverb as a send, not an insert. One reverb on a send (FX Sends) serves every "
     "track for no insert slots; as an insert it takes 4 of the 8 for one track.\n"
     "Good inserts: an amp on a guitar or bass track, a compressor or drive on drums, an EQ or "
     "filter to shape one sound, a phaser or chorus on keys.\f"
     "Fields: Insert (which of the track's chain), Category, Effect, then K1-K3 (or the wheel) "
     "for its three controls.\n"
     "Inserts are saved with the project, and play in exports and mixdowns."},
    {OS_MASTER, -1, "Master Effects",
     "The last stage before the outputs, never resampled.\n"
     "EQ (after the Pultec) then Comp: a dbx 160 or the SP-303 Vinyl Sim.\n"
     "B1 EQ page.\nB2 Comp page.\nB3 bypasses both.\f"
     "Pultec: boost and atten together on the low end lift the bottom and dip just above it.\n"
     "dbx 160: Threshold, Ratio, OverEasy (a soft knee), Output.\f"
     "Vinyl Sim: Comp pumps, Noise adds crackle and hiss, Wow Flut wobbles the pitch.\n"
     "Tip: they suit loud, normalized material."},
    {OS_MUTE, -1, "Track Mute",
     "Pads mute tracks, solo them, mute or solo single pads, or queue sequences.\n"
     "B1 <Type> changes which. PAD BANK shows the next sixteen.\f"
     "<All Off> clears the type shown.\n"
     "SHIFT+MUTE (UNMUTE ALL), on any page: every track unmuted.\n"
     "Next Seq: with the wheel on every type: the sequence that plays next."},
    {OS_SONG, -1, "Song",
     "Sequences chained into a song. Press pads to add steps: A01 is sequence 1, B01 is 17. "
     "Each goes in at the selected step, and the selection moves on.\f"
     "The wheel or -/+: a step's sequence and Reps. On (end) the wheel adds a step.\n"
     "B1 Delete.\nB2 Insert (another of the step).\nB3 Convert: the song made into one sequence, in the first empty one (asks first).\f"
     "PLAY plays the song from the selected step; it ends after the last step.\n"
     "Opened with Shift+Pad 12."},
    {OS_STEP, -1, "Step Edit",
     "The active track's notes at one moment (Now), one at a time, as on the MPC3000.\n"
     "- and + step Now by the Step note value and play what is there.\f"
     "B2 and B3 jump to the previous and next notes. Stopped, a pad puts a note in at Now. B1 "
     "deletes the selected note.\n"
     "ENTER moves through Now, Step and each note's pad, velocity and length.\f"
     "Shift + wheel on Now moves a tick at a time.\n"
     "Opened with Shift+SEQ.\f"
     "The Grid (Shift+SEQ again): a row a pad, the steps across. The wheel picks the row; a pad "
     "selects its row only. - and + step Now. B1 <Insert> puts a note in (full level); on a "
     "note it reads <Delete>.\f"
     "Shift + wheel moves Now a tick at a time. On a note, Shift + B1 <Ghost> makes it soft (48), "
     "<Full> puts it back to 127. B2/B3 a page."},
    {OS_WARP, -1, "Warp",
     "Changes the selected pad's length and pitch apart. K1 length, K2 pitch, K3 fine.\n"
     "Bars: how long the loop is, for its BPM. <Seq BPM> fits it to the sequence.\f"
     "Each change renders again and plays. <To Pad> puts it on the pad; the original stays "
     "in memory.\n"
     "Shift+Pad 15 again: S-Series Timestretch, the Akai S1000's TIME page.\f"
     "Start/End: the zone stretched. Time Factor 25-2000%. Mode CYCLIC: a fixed Cycle Len in "
     "samples (<AutoC> finds one from the pitch), for single notes.\f"
     "INTELL: splices chosen by the sound; QUAL how many samples go in or out at a time, WIDTH "
     "the crossfade. 90 and 90 suit most things. <GO> puts it on the pad."},
    {OS_PREFS, -1, "Additional Settings",
     "Button lights: red, white, pink or off. Pad lights: blue, red, white or off; lit pads dim "
     "while pressed, more with pressure.\n"
     "Pad Thresh: the lightest pad hit that counts (1-60, 12 to start). Raise it if pads "
     "trigger by themselves.\n"
     "Screen: the colours of other machines.\f"
     "Sequencer Res: the machine the timing and swing are taken from. MPC 3000, MPC60 and "
     "MPC 2000: 96 PPQN, swing 50-75. MPC 3000 moves in 2-tick steps at 1/16; MPC60 in 1-tick "
     "steps, as measured from its grooves; the 2000 rounds its swing down. SP12: 24 PPQN, "
     "swing 50, 54, 58, 63, 67, 71, as measured from SP-1200 grooves.\n"
     "Start BPM, Start Bars, Start TC and Start Swing: the tempo, the length of every "
     "sequence, Timing Correct and swing the MPC 600 S starts with at power-on.\f"
     "B3 <Page 2>:\n"
     "Loop Conv (Live Loop Conversion): IMMEDIATE converts a Live Looper take while its pass "
     "is recorded, so it drops in at its own rate and bits a few ms after the pass. AFTER 1ST "
     "LOOP drops the take in as recorded (44.1k, 16-bit) and converts it while it plays: the "
     "converted one takes over from the next pass. Good for longer takes.\n"
     "Root Auto-Detect: ON (to start) finds a sound's Root Note (S600 Main) when it is loaded "
     "or recorded into an S600 program. OFF: it stays at C4.\f"
     "Recording Starts With: the settings every recording page starts with (Sample New Sound, "
     "Audiotrack Record, Convert Sample, Resample, Live Looper). Changing one sets it on them "
     "all; each page can still be changed on its own.\n"
     "Sample Rate (29.76 kHz to start): the wheel steps the slider's rates (7, 11.02, 22.05, "
     "26.04, 29.76, 44.1 kHz), then SET. On SET, push the wheel in to change the number to any "
     "rate (half-kHz steps); push again when done.\f"
     "Bit Rate (12 to start), Anti-Aliasing and Normalize (ON to start).\n"
     "Input Thru: ALWAYS ON (to start) hears the input at power-on; OFF starts with it off, "
     "to turn on by hand (Monitor on Sample New Sound).\n"
     "Rec Mode: STEREO (to start), MONO LFT, MONO RGT or MONO LR. Convert Sample always "
     "starts in MONO LR.\f"
     "Knob Mode: how K1-K3 and the slider take over a value.\n"
     "CATCH (to start): nothing moves until the knob reaches the value.\n"
     "JUMP: the value goes to where the knob is, at once.\n"
     "SCALE: the value moves from where it is, in step with the knob, so the two meet at the "
     "ends of its travel."},
    {OS_DISK, -1, "Save/Load",
     "SAVE, LOAD or EXPORT. Types: project (everything), program, sample, settings, autoload "
     "(loaded at power-on), FX chain (the input chain), arp pattern (a USER step pattern), "
     "track FX (the Track/Mixer send effects), S600 osc (the S600 waveforms), sequence (one "
     "sequence and all its tracks; Seq: which).\f"
     "LOAD only: MIDI FILE (.mid), MPC 2500 SEQUENCE (an MPC 2500 .ALL or .SEQ; MPC 1000 and 500 "
     "ones too) and MPC 2500 PROGRAM (its .PGM, with its WAVs beside it). MPC 2500 PROJECT (also SAVE: a folder an MPC 2500 loads, with its .PRJ, .ALL, programs and WAVs. LOAD: an MPC 2500 .PRJ; MPC 1000 and 500 ones too: every program into the next empty program, then every sequence into the next empty sequence). Into: the sequence or program they go into.\f"
     "MIDI FILE and MPC 2500 SEQUENCE: choose the sequence (an .ALL holds several), then the tracks: push "
     "puts a track in or out, SHIFT+push makes it DRUM or KEYS, B2 all or none, B3 loads. They "
     "go onto the first empty tracks. An empty sequence takes the file's tempo, time signature "
     "and length.\f"
     "DRUM tracks go onto the pads: an MPC's notes by its program file beside the .ALL, a MIDI "
     "file's by Pad Notes; each plays its MPC program, if loaded (MPC 2500 PROGRAM first). KEYS tracks play the program's S600 Instrument, or are MIDI tracks "
     "on an MPC program.\f"
     "Names: the wheel sets the letter, - and + move along it.\n"
     "Folders: each type opens in its own; .. goes up, anywhere on the card. Folders show as "
     "/NAME.\f"
     "SAVE and EXPORT:\nB1 <New Dir> makes a folder here.\nB2 <Open> goes into a folder (or "
     "takes an item's name).\nB3 saves or exports here.\f"
     "LOAD:\nB1 renames (on a folder: <Up>, out of this one).\nB2 deletes (asked first).\nB3 loads, or opens a folder.\f"
     "EXPORT: a sequence or song as a folder of WAVs: each track with notes (mono or stereo), "
     "and the stereo mix with Master FX.\n"
     "MIXDOWN SEQ, MIXDOWN SONG: the stereo mix alone, one WAV.\f"
     "Shift+SAMPLE SELECT opens this on SAVE SAMPLE."},
    {OS_BROWSER, -1, "Sample Select",
     "Load WAV files. Wheel moves, ENTER or B3 loads onto the selected pad, B1 goes up a "
     "folder, B2 preview on or off.\n"
     "Shift+B1 makes this folder the one it opens in."},
};
#define NHELP ((int)(sizeof HELP / sizeof HELP[0]))

static int section_of(const struct os *s, int screen) {
    for (int i = 0; i < NHELP; i++)
        if (HELP[i].screen == screen &&
            (HELP[i].record_page < 0 || HELP[i].record_page == (screen == OS_MULTI ? s->multi_page : s->record_page)))
            return i;
    return -1;
}

// A help page as shown: its lines, and whether in the half-size font.
struct help_page {
    int small, n;
    char line[PAGE_LINES][SMALL_COLS + 1];
};

// One paragraph wrapped at `cols`, whole words; returns its lines.
static int wrap_par(const char *p, int len, int cols, char out[][SMALL_COLS + 1], int max) {
    int n = 0;
    while (len > 0 && n < max) {
        int take = len < cols ? len : cols;
        if (take < len) { // break at the last space that fits
            int sp = take;
            while (sp > 0 && p[sp] != ' ') sp--;
            if (sp > 0) take = sp;
        }
        memcpy(out[n], p, (size_t)take);
        out[n][take] = 0;
        n++;
        p += take, len -= take;
        while (len > 0 && *p == ' ') p++, len--;
    }
    return n;
}

// A line that is one of a list of keys (Shift+B1 ..., K2 ..., Hold SAMPLE ...).
static int key_line(const char *p) {
    static const char *const KEYS[] = {"Shift+", "B1", "B2", "B3", "K1", "K2", "K3", "Hold "};
    for (int i = 0; i < (int)(sizeof KEYS / sizeof KEYS[0]); i++)
        if (!strncmp(p, KEYS[i], strlen(KEYS[i]))) return 1;
    return 0;
}

// The section's text as pages. "\f" starts a page of its own; "\n" a new tip or topic, with a
// blank line before it, except under a heading ("Hidden keys:") and between lines of a list of
// keys. A section with a page too long for the normal size is all in the half-size font, and
// only a page still too long then goes on over more pages, between topics.
static int layout_as(const char *text, struct help_page *pages, int small, int *overflow);

static int layout(const char *text, struct help_page *pages) {
    int over = 0, np = layout_as(text, pages, 0, &over);
    if (!over) return np;
    np = layout_as(text, pages, 1, &over);
    // In the small font pages have room: one that fits under the one before joins it.
    int out = 0;
    for (int i = 0; i < np; i++) {
        if (out > 0 && pages[out - 1].n + 1 + pages[i].n <= SMALL_ROWS) {
            struct help_page *a = &pages[out - 1];
            a->line[a->n++][0] = 0;
            for (int k = 0; k < pages[i].n; k++) memcpy(a->line[a->n++], pages[i].line[k], sizeof a->line[0]);
        } else {
            if (out != i) pages[out] = pages[i];
            out++;
        }
    }
    return out;
}

static int layout_as(const char *text, struct help_page *pages, int small, int *overflow) {
    int np = 0;
    *overflow = 0;
    const char *chunk = text;
    while (*chunk && np < MAX_PAGES) {
        const char *end = strchr(chunk, '\f');
        if (!end) end = chunk + strlen(chunk);
        {
            int cols = small ? SMALL_COLS : TEXT_COLS, rows = small ? SMALL_ROWS : TEXT_ROWS;
            // The chunk's paragraphs, each wrapped, a blank line between them.
            char lines[64][SMALL_COLS + 1];
            int starts[32], npar = 0, n = 0;
            const char *prev = 0, *prev_end = 0;
            for (const char *p = chunk; p < end && n < 60;) {
                const char *e = memchr(p, '\n', (size_t)(end - p));
                if (!e) e = end;
                int heading = prev && prev_end > prev && prev_end[-1] == ':';
                if (npar && !heading && !(key_line(prev) && key_line(p))) lines[n++][0] = 0;
                starts[npar++] = n;
                prev = p, prev_end = e;
                n += wrap_par(p, (int)(e - p), cols, lines + n, 60 - n);
                p = e + (e < end);
            }
            if (n > rows && !small) *overflow = 1;
            // Fill pages, breaking only between paragraphs (a paragraph too long breaks anywhere).
            int at = 0;
            while (at < n && np < MAX_PAGES) {
                int take = n - at;
                if (take > rows) {
                    take = 0;
                    for (int k = 0; k < npar; k++) // the last paragraph start that still fits
                        if (starts[k] > at && starts[k] - at <= rows) take = starts[k] - at;
                    if (take <= 0) take = rows;
                }
                struct help_page *pg = &pages[np++];
                pg->small = small, pg->n = 0;
                for (int k = 0; k < take; k++)
                    if (pg->n || lines[at + k][0]) snprintf(pg->line[pg->n++], SMALL_COLS + 1, "%s", lines[at + k]); // no blank first line
                at += take;
            }
        }
        chunk = *end ? end + 1 : end;
    }
    return np;
}

int help_open(struct os *s, int page) {
    int screen = s->screen == OS_HELP ? s->help_return : s->screen;
    if (section_of(s, screen) < 0) return 0;
    if (s->screen != OS_HELP) s->help_return = s->screen;
    s->screen = OS_HELP;
    s->help_page = page;
    return 1;
}

static int64_t clock_ms(const struct ui *u) {
    if (u->now_ms) return u->now_ms;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// The helper, bottom right: four taps of the foot, then the pointer at the text, round again.
static void draw_helper(struct ui *u, int64_t now) {
    static const int SEQ[] = {0, 1, 0, 1, 0, 1, 0, 1, 2, 3, 2, 3, 2, 3};
    int step = (int)(now / 280 % (int64_t)(sizeof SEQ / sizeof SEQ[0]));
    const char *const *f = HELPER[SEQ[step]];
    int x0 = UI_W - HELPER_W * 2 - 2, y0 = BOTTOM_ROW * CELL_H - HELPER_H * 2 - 2;
    for (int y = 0; y < HELPER_H; y++)
        for (int x = 0; x < HELPER_W; x++)
            if (f[y][x] == '#') lcd_dot(u, x0 + x * 2, y0 + y * 2, LCD_FG);
}

void help_frame(struct os *s, struct ui *u, const struct input *in) {
    int sec = section_of(s, s->help_return);
    if (sec < 0 || input_down(in, IN_B2)) { // <Exit>
        s->screen = s->help_return;
        ui_wake(u, 1);
        return;
    }
    static struct help_page pages[MAX_PAGES];
    int np = layout(HELP[sec].text, pages);
    if (input_down(in, IN_B1) && s->help_page > 0) s->help_page--;
    if (input_down(in, IN_B3) && s->help_page + 1 < np) s->help_page++;
    int d = in->encoder;
    if (d) s->help_page = s->help_page + d < 0 ? 0 : s->help_page + d >= np ? np - 1 : s->help_page + d;
    if (s->help_page >= np) s->help_page = np - 1;
    if (s->help_page > 0) lcd_text(u, 0, KEY_ROW, "<Prev>");
    if (s->help_page + 1 < np) lcd_text(u, 20, KEY_ROW, "<Next>");
    lcd_text(u, 10, KEY_ROW, "<Exit>");
    char title[48];
    snprintf(title, sizeof title, "Help:%s %d/%d", HELP[sec].title, s->help_page + 1, np);
    lcd_rule(u, TITLE_ROW, title);
    const struct help_page *pg = &pages[s->help_page];
    for (int i = 0; i < pg->n; i++) {
        if (pg->small) lcd_small_text(u, LCD_X, SMALL_Y + i * 10, pg->line[i], LCD_FG);
        else lcd_text(u, 0, 3 + i, pg->line[i]);
    }
    draw_helper(u, clock_ms(u));
    lcd_rule(u, BOTTOM_ROW, 0);
    ui_wake(u, 140); // she keeps moving
}

// Tests: how many pages a screen's help has, and whether page `page` is in the small font.
int help_pages(struct os *s, int screen, int page, int *small) {
    int sec = section_of(s, screen);
    if (sec < 0) return 0;
    static struct help_page pages[MAX_PAGES];
    int np = layout(HELP[sec].text, pages);
    if (small) *small = page >= 0 && page < np ? pages[page].small : 0;
    return np;
}

// Tests: every page of a screen's help, as laid out, to stdout.
void help_dump(struct os *s, int screen) {
    int sec = section_of(s, screen);
    if (sec < 0) return;
    static struct help_page pages[MAX_PAGES];
    int np = layout(HELP[sec].text, pages);
    for (int p = 0; p < np; p++) {
        printf("--- %s %d/%d%s\n", HELP[sec].title, p + 1, np, pages[p].small ? " (small)" : "");
        for (int i = 0; i < pages[p].n; i++) printf("|%s\n", pages[p].line[i]);
    }
}
