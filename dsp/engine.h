#pragma once
// The AmbientForce engine (docs/CONCEPT.md 4 and 8): the harmony brain, the four strata (Ground,
// Bloom, Air, Weather), Echo and Space, Memory, and the output stage, behind the interface the plugin
// drives (MIDI at sample offsets, the transport once per block, a Patch when a parameter changes,
// Weather's source and Remember once per block).
//
// Routing (CONCEPT.md 4.1). A key goes through Input (mapInput) once, on its way down; the
// harmony keeps the key and its mapped note together, so a key's note-off finds its note even if
// the patch changed in between. Then every stratum hears it by its Listen mode:
// - Bloom, Notes: the chord on the mapped note (buildChord, led from the chord Bloom played last
//   when Leading is on; with Chord Off the single note), owned by the key. Harmony: it moves to
//   the harmony's chord whenever that changes (Harmony::version()), and lets go when the memory
//   forgets it. Free: it moves to the tonic chord (its Chord type, a triad under Chord Off) from
//   the first note on, and moves again when the key, scale, chord or voicing change.
// - Ground, Notes: the lowest note held (fading when none is). Harmony: the harmony's root. Free:
//   the tonic, from the first note on.
// - Air, Notes: each key down plays its mapped note on Air (Air::play: struck at the key's sample,
//   its pan by its pitch, its velocity through Vel), and the generator generates nothing (a loop
//   still replays). Harmony: the generator generates from the harmony's chord while there is one
//   (setChord(current(), root >= 0), given when it changes). Free: from the tonic chord, from the
//   first note on. While Air is off (level 0, as in Init, or muted) it is given neither chords nor
//   notes: it isn't rendered, a chord change would work its generator's candidates out for nothing
//   (twelve times in a re-strike of six keys), and a note struck into it would ring from where it
//   stood when Air was turned up. Turned up, it takes the chord there is then.
// - Weather, Notes: its gate open while any key is held (a finger, the pedal or Hold), To Key Chord
//   on the held notes' pitch classes. Harmony: open while the harmony has a chord, on its pitch
//   classes. Free: open from the first note until the engine sleeps, on the tonic chord's. Shut, it
//   fades out over Weather::kGateS, keeping the chord it had.
// - Split (split >= 0): a key at or above it plays Air alone, its mapped note, whatever Air's
//   Listen: it wakes the engine as any note does (Free strata start), but enters nothing else (not
//   the harmony, Bloom, Ground or Weather's notes), and its note-off finds nothing to let go. Keys
//   under it are as they would be without Split, but for Air: on the notes, Air plays only the keys
//   over Split (so a split keyboard is Air's melody above and the rest below).
// Nothing sounds before the first note-on, Free strata included: the engine is asleep. It wakes
// at the first note-on and stays awake through note-offs, until Stop or reset() put it to sleep.
//
// Keys, the pedal and Hold. A key up while the pedal (CC 64) is down is held by the pedal until
// it comes up; with Hold on it is latched instead. The first note-on after every finger has left
// the keys starts a new chord and lets the latched keys go, after the new chord has started
// (Bloom's play() before release(), so the notes the two chords share carry on); a key pressed
// while a finger is still down joins the chord. A key held either way still counts as held for the
// harmony: the memory's bars count from when the chord is let go, not from when the fingers left
// it, and Ground in Notes mode stays on the chord the pedal or Hold keeps sounding. (Under Chord
// Off, where the harmony's chord is the notes held, a new chord's latched keys leave the harmony
// before its first key goes down, or they would stay in its chord.) The harmony keeps 16 keys:
// full, a new key lets the oldest one the pedal or Hold keeps go first (a pedal down through a
// long phrase), while 16 fingers keep theirs and the new key isn't heard there. Hold turned off
// lets the latched keys go (to the pedal, if it is down). CC 123 lets every key go, held by a
// finger, the pedal or Hold, but the engine stays awake: what the harmony remembers goes on
// sounding.
//
// Listen changed while sounding: Ground is given its new target at once (gliding by Gravity, or
// fading when the new mode has none). Bloom moved to Harmony or Free moves to that chord at once,
// the notes in common carrying on; moved back to Notes, the keys held (by a finger, the pedal or
// Hold) play their chords again, oldest first, before the harmony's or the tonic's chord lets go,
// so Bloom and Ground agree. Air is given its new chord and whether it generates at once; Weather's
// gate opens or shuts at once. Within one setPatch() Weather's gate is set before its level (on
// purpose, weather.h asks the engine to choose): a patch that shuts the gate and raises the level at
// once (a preset loaded with Weather on the notes and no key held) leaves Weather silent, where the
// level first would play two seconds of the gate's fade out of a cloud its Listen doesn't want. A
// gate opening is the same either way (it fades in from -60 dB).
//
// Stop (CONCEPT.md 7.4): the transport stopping (playing to stopped, from setTransport) or a
// suspend that resumes within kStopWindowS applies On Stop: Keep does nothing; Fade fades the
// output to silence over kFadeS (linear in dB down to -60 dB), then resets and sleeps; Cut resets
// at once. A note-on or the transport starting during a fade turns it round: the output comes
// back up at kRecoverDbPerS. On Stop changed during a fade applies at once: Keep turns it round,
// Cut resets. A longer suspend resets. CC 120 and reset() silence everything at once, forget the
// harmony and the keys, and sleep.
//
// The mix: Ground and Bloom each render their dry (panned by groundPan / bloomPan, equal power,
// unity in the middle) into the dry bus and add their sends into the Space bus (Ground at
// groundSpace, Bloom at bloomSpace). Bloom's send is 0 whenever Space can't carry a tail (the
// return or Bloom's send at 0, or Freeze on: a frozen reverb takes no input): it then releases as
// Tail Voice. Ground's is 0 with the return at 0 too, so a Space nobody hears isn't run. With
// Bloom's Tail on Space, the reverb's decay is held at Bloom's Release or longer (in Abyss, which
// rings four times its Decay, at a quarter of it), so the tail a voice hands off carries on. The
// volume, the return and the pans glide in a straight line over 10 ms from where the next control
// step finds them changed, a step a sample carried from one piece to the next, so a MIDI event
// that cuts a piece short never makes one jump.
//
// M2's mix (all of it off in Patch{}, Init, which plays 0.0.2's samples bit for bit):
// - Air and Weather render as Ground and Bloom do, panned by airPan / weatherPan, their Space sends
//   at airSpace / weatherSpace (0 with Space's return at 0, as Ground's). Weather is given Bloom's
//   dry peak over the piece for Duck (Bloom then renders into its own buffer), and its source: the
//   plugin's (setWeatherSource), or with weather.memory Memory's remembered 16 s.
// - Echo (plan decision 2): each stratum's dry, after its level and before its pan, goes into the
//   Echo bus at its Echo send (groundEcho, bloomEcho, airEcho, weatherEcho), gliding as the pans do.
//   Bloom's tail handoff isn't echoed (its boost is Space's). A stratum centred and sending nothing
//   to Echo still renders straight into the bus; one that sends renders into its own buffer first.
//   Echo runs while a stratum sounding sends to it, or while it isn't silent(); its return goes into
//   the mix at echoReturn and into Space's send bus at echoSpace (both gliding). The sends to Echo
//   are 0 where nobody would hear it (echoReturn 0, and echoSpace or Space's return 0), Echo's to
//   Space where Space's return is 0, so an Echo nobody hears isn't run. When it runs after the
//   engine skipped it, it is told for how long (Echo::rest()), so its duck has fallen as far as it
//   would have: the first repeats after a pause come as loud as a Delay's run on through it
//   (echo.h; the engine counts the gap on its sample count, idle and asleep alike).
// - Memory records its tap once a piece has been through the guard (it never records a piece the
//   guard zeroes): Strata, the four strata's dry sum as it is in the bus (panned), or Output, the
//   dry and Echo's and Space's returns, before the tilt. It records only while the engine runs DSP
//   (awake and not idle). remember() applies at the next control step's start, so Weather (on
//   Memory) renders from the new ring in that very piece, before Memory's next write() records
//   over the old one (memory.h asks for that order: Remember, Weather, write). Asleep no control
//   step runs, and a Remember applies at the next render() at once.
//
// The output: dry + Echo's return x echoReturn + Space's x spaceReturn -> tilt -> make-up and
// volume -> the non-finite guard -> limiter -> Stop's fade.
// - Tilt: one first-order shelf pivoting at 800 Hz, the highs up and the lows down by 6 dB at
//   tilt 1 (and the other way at -1), 0 dB at the pivot. Bypassed at 0.
// - Make-up: a fixed kMakeUpDb, after the mix (the strata's levels, their sends and Rise keep their
//   own calibration: Ground's headroom, Bloom's voice gain of 0.25, Rise's -28 dBFS), so that Init,
//   which is Patch{}, plays the demo phrase (tools/phrase.h) at the family's -16 LUFS at the
//   volume's default -6 dB; the factory presets are matched at that default or under it (down to
//   -10.5 dB). It rides in the volume's gain, one multiply.
// - Volume before the limiter, so the ceiling holds at every volume: the knob reaches +6 dB, 12 dB
//   over the presets, where the limiter takes the extra rather than the output passing -1 dBFS.
//   (After the limiter, +6 dB would put a limited peak at +5 dBFS.)
// - The guard: any sample that isn't finite zeroes the whole render() call, resets every DSP
//   state (Ground, Bloom, Air, Weather, Echo, Space, the output's filters, the ring Memory records;
//   what Memory remembered stays, plan decision 10) and is counted; the keys and the harmony
//   stay, so Harmony and Free strata come back by themselves. It looks before the limiter, whose
//   soft clip would turn an infinity into a finite peak.
// - The limiter: no lookahead. A stereo-linked gain computer (1 ms attack, 150 ms release) holds
//   the peak at kKnee (0.95 of the ceiling); what the attack lets through goes into a soft clip
//   (softclip(), a tanh-like curve) from kKnee up to kCeiling, which it never passes: the output
//   stays at or under -1 dBFS (0.8913) whatever comes in. It keeps its gain as the distance under
//   1, so the release (a float step of 1.5e-4 of that distance) never stalls short of 1; with no
//   peak over the knee in a piece and the gain within 0.1% of 1, it lands on 1 (a step of 0.009
//   dB). Under the knee, with the gain back at 1, it only looks.
//
// Control rate: every kChunk samples the harmony's memory advances at the transport's tempo, the
// routes are looked at again (the memory may have forgotten the chord), Stop's fade and the tilt
// take a step. The steps sit on the sample count's multiples of kChunk, so the host's blocks
// (MPC's 128) never cut one; only a MIDI event does, at its sample: the same events play the same
// samples whatever the blocks (multiples of kChunk; checked with everything on). The strata, Echo
// and Space are rendered a piece at a time: they read their tables (TableSet::get) and step their
// own controls at every call. Before each piece Ground, Bloom and Air are given MPC's tempo and the
// engine's one beat count at its first sample, for their synced cycles (Ground's Breath, the sways,
// Air's synced loop): MPC's position while the transport plays, on from where it was at the tempo
// while it is stopped. The count runs on the sample count, asleep or awake, so the strata keep to
// one grid whether they sound or not.
//
// Idle: asleep, or awake with nothing to hear (Ground !audible(), Bloom with no voice in use, Air
// neither audible() nor generating(), Weather !audible()), Echo::silent() and Space::silent():
// render() writes zeros and runs no DSP. Each stratum may be skipped only by its own contract:
// Ground and Air (their voices) stand still where they were, Weather holds no source while
// !audible(), Echo and Space start afresh by themselves after a silence however it began. Air's
// generator is rendered while it may strike (its level up, generating or looping), so a gap between
// its notes doesn't stop it. Space can stay unsilent for minutes (Abyss at Decay 30), and a set()
// that lengthens its reach can make it unsilent again with no input: it then simply runs that much
// longer.
//
// Cost, as ARM instructions per 128-sample block (make arm-icount: qemu's count, the device's flags,
// the plain build; M2's cases play the engine itself, tools/bench.cpp says why): asleep 1.2k. The
// bench's M1 cases each count 5.8k more than in 0.0.2, with Air, Weather and Echo off: Memory
// recording the output (3.6k, memory.h) and the new routing. Init holding a triad 203.0k; the worst
// case (six voices of unison 2 with FM, every Ground partial and Body, Abyss with shimmer) 400.3k,
// 418.2k in its re-strike's block. Air alone (Felt ringing in six voices at Decay 20, Density 60,
// Loop on, Init's Space) 149.0k; Weather alone (16 grains, +12, To Key Chord, high-pass, tilt,
// Duck) 181.4k. M2's worst case (`worst m2`: that worst case with Air and Weather so, every Echo
// send open into Echo at Ping-Pong, wow, Diffuse and duck 1, feedback 0.9, Memory, a Remember with
// each re-strike) 531.9k, 570.0k in its re-strike's block (the Remember 23k of it): Weather 60.5k,
// Air 38.4k (its six keys struck again every 2 s, panned, sent to Echo), Echo 32.8k with the four
// sends, each the worst case less it. 0.0.2's worst counted 394.5k: M2 adds 137.4k, where the plan
// budgeted 128k. At its 0.0268 points of p99 a thousand that is 14.25 points against the 15% gate
// (device: pending), under the 14.5 at which the plan's caps start (Bloom's unison 2 first).
// Spikes, counted by hand (docs/PERFORMANCE.md): six Kalimba plucks struck from C1 cost 210.6k in
// their block and 10.2k a block for the ten their bursts take (airvoices.h: bounded, Air's share
// being for ringing); a phrase after a silence 15.8k more in its first block than in the blocks
// after (Echo's fresh start, echo.h, and the note's).
//
// Real-time rules: everything is allocated in the constructor (Space's buffers, about 870 KB; Echo's
// lines, 2.8 MB; Weather's copy room, 86 KB; Memory's two rings, 9.9 MB). Nothing on the audio
// thread allocates, locks or throws.
#include "air.h"
#include "bloom.h"
#include "echo.h"
#include "grainsrc.h"
#include "ground.h"
#include "harmony.h"
#include "lifetime.h"
#include "memory.h"
#include "space.h"
#include "weather.h"

#include <cstdint>

namespace af {

// The volume at or below which the output is off: the knob's text shows "-inf dB" from here.
constexpr float kVolumeOffDb = -59.5f;

// What Stop does to a sounding landscape (docs/CONCEPT.md 7.4).
enum OnStop : int { OS_KEEP, OS_FADE, OS_CUT, OS_COUNT };
inline constexpr const char* kOnStopNames[] = {"Keep", "Fade", "Cut"};   // the surface's (harmony.h says why)
static_assert(sizeof kOnStopNames / sizeof *kOnStopNames == OS_COUNT, "a name per On Stop");

// Init's Space: a slow hall for pads, not the Reverb's own defaults (EffectForce's, for any input).
constexpr Space::Params initSpace() {
    Space::Params s;
    s.reverb.mode = Reverb::HALL;
    s.reverb.size = 0.6f;
    s.reverb.decayS = 8.0f;
    s.reverb.predelayMs = 30.0f;
    s.reverb.dampHz = 6000.0f;
    s.reverb.lowCutHz = 120.0f;
    s.reverb.mod = 0.4f;
    s.reverb.width = 1.0f;
    s.rise = 0.2f;
    return s;
}

// Everything the engine plays, in real values (seconds, Hz, gains): plugin/patch_map.cpp builds it
// from the parameters. Levels and sends arrive as gains, the knobs' audio taper (gain = knob^2) being
// the patch map's. The defaults are Init's (the surface's defaults through the patch map, which
// test/params_test.cpp holds them to): the levels and sends are the default knobs squared.
struct Patch {
    float volumeDb = -6.0f;
    float tilt = 0.0f;            // -1..1: ±6 dB at the extremes, pivot 800 Hz
    HarmonyPatch harmony;
    bool hold = false;            // latch: chords stay until the next one
    int onStop = OS_FADE;
    GroundPatch ground;
    float groundSpace = 0.4f * 0.4f;   // Ground's send: the knob at 40%
    float groundPan = 0.0f;
    BloomPatch bloom;
    float bloomSpace = 0.5f * 0.5f;    // Bloom's send: the knob at 50%
    float bloomPan = 0.0f;
    Space::Params space = initSpace();
    float spaceReturn = 0.8f * 0.8f;   // the return's level: the knob at 80%
    // M2. Everything new is off here (the levels and the Echo sends at 0), so Init plays as 0.0.2 did.
    float groundEcho = 0.0f, bloomEcho = 0.0f;                         // the strata's Echo sends (gains)
    AirPatch air;           float airSpace = 0.5f * 0.5f, airEcho = 0.0f, airPan = 0.0f;
    WeatherPatch weather;   float weatherSpace = 0.3f * 0.3f, weatherEcho = 0.0f, weatherPan = 0.0f;
    Echo::Params echo = initEcho();                                    // Echo's defaults (echo.h)
    float echoReturn = 0.7f * 0.7f;   // Echo Level: the return into the mix
    float echoSpace = 0.3f * 0.3f;    // Echo's own send into Space
    int memoryTap = MT_OUTPUT;
    int split = -1;                   // Split: keys at and above this play Air as melody; -1 off
};

class Engine {
public:
    static constexpr double kStopWindowS = 0.25;  // a suspend resumed within this is a Stop, longer a reset
    static constexpr float kFadeS = 8.0f;         // On Stop's Fade, to -60 dB
    static constexpr float kRecoverDbPerS = 30.0f;   // a fade turned round comes back this fast
    static constexpr float kMakeUpDb = 8.9f;      // the output's make-up gain, with the volume (above)
    static constexpr float kCeiling = 0.891f;     // -1 dBFS: the output never passes it
    static constexpr float kKnee = 0.95f * kCeiling;   // the limiter holds peaks here; the soft clip above

    // What the tests (and nothing else) look at.
    struct Info {
        bool awake = false;           // a note-on has woken it, and no Stop or reset put it to sleep
        bool idle = false;            // the last piece (a control step, or what an event left of it) ran no DSP
        bool fading = false;          // On Stop's Fade under way
        float fadeDb = 0.0f;          // where it is (0: none)
        bool suspended = false;       // suspend() without its resume() (the plugin calls both at once; the tests apart)
        int groundTarget = -1;        // the note Ground was last given (-1: none)
        bool groundAudible = false;
        int bloomActive = 0;
        int harmonyRoot = -1;
        uint32_t harmonyVersion = 0;
        uint32_t guards = 0;          // blocks the non-finite guard zeroed
        uint64_t samples = 0;         // rendered so far
        float limiterGain = 1.0f;
        bool limiting = false;        // the last piece ran the limiter's gain computer
        bool gliding = false;         // the volume, a return, a pan or an Echo send is still on its way
        float spaceDecayS = 0.0f;     // the decay Space was given (the tail's hold applied)
        int airActive = 0;            // Air's voices ringing
        uint64_t airStrikes = 0;      // notes Air has struck (generated, replayed and played)
        bool weatherAudible = false;
        bool weatherGate = false;     // what Listen gives Weather's gate
        uint64_t echoRuns = 0;        // pieces Echo has run in
        bool echoSilent = true;
        bool remembered = false;      // Memory holds a Remember
        float memoryFill = 0.0f;      // 0..1: of 16 s, the ring recording
        uint32_t memoryGeneration = 0;   // +1 at every Remember
    };

    explicit Engine(const TableSet& tables);

    void setPatch(const Patch& p);           // between render() calls
    void noteOn(int note, int velocity);     // velocity 0 = note off
    void noteOff(int note);
    void pitchBend(float) {}
    void sustain(bool down);
    void allNotesOff();                      // CC 123: every key let go; the engine stays awake
    void reset();                            // CC 120: silence now, the harmony forgotten, asleep
    void controller(int, int) {}
    void aftertouch(float) {}
    void polyAftertouch(int, float) {}
    void resetControllers() { sustain(false); }   // CC 121: the pedal back up
    void seed(uint32_t s);                   // per instance: Ground's and Bloom's random numbers
    // MPC's tempo and position (quarter notes), once per block before render(). playing falling to
    // false is a Stop.
    void setTransport(double bpm, double beats, bool playing, bool beatsValid);
    // The host stopped processing, and started again `awayS` seconds later: within kStopWindowS
    // that is a Stop (On Stop applies), longer a reset. A resume() without a suspend() does nothing.
    void suspend();
    void resume(double awayS);
    // Weather's source, from the plugin's loader, once a block before render(): what the grains read
    // unless Weather is on Memory (weather.memory), where they read memory().remembered(). nullptr:
    // none. Weather keeps pointing into it from block to block while it is audible, and the first
    // render() given another (or none) copies from the old one what its fading grains still read
    // (dsp/weather.h): so the old one must stay readable through the first block that gives the new
    // one. The plugin's side of that (plugin/loader.h's graveyard; Task 10 wires it), every block:
    //   loader.blockStart();  engine.setWeatherSource(<live(slot)'s GrainSource>);  render ...;
    //   loader.blockDone(engine.holdsSource());
    // blockStart() before live(), exactly one blockDone() per blockStart(), holdsSource() asked
    // after the block's last render() (Weather may let go of its source during it), live() on the
    // audio thread only. Keep, pressed through setParameter, must not reach the loader's post()
    // from the audio thread (it locks and allocates). At a suspend, with Weather silent
    // (holdsSource() false), telling the loader nothing is held lets it free a source picked while
    // MPC isn't calling the track. The engine reads the pointer only while it renders Weather
    // (audible()), so one the loader has freed while Weather was silent is never looked at.
    void setWeatherSource(const GrainSource* src) { weatherSrc_ = src; }
    // Whether anything still points into the plugin's source once the block is over (the loader's
    // `holds`): Weather audible() and its last render given the plugin's source, ready (not Memory's,
    // not none). False, the loader may free a replaced source as soon as no block runs.
    bool holdsSource() const { return weather_.audible() && weatherHolds_; }
    // Remember (the plugin's button, before a block): Memory's ring recording becomes Weather's
    // remembered source at the next control step (Memory::remember(); refused within 2 s of the
    // last, with under 0.5 s recorded, or while Keep writes: Info says whether the generation moved).
    void remember() { rememberAsked_ = true; }

    void render(float* outL, float* outR, int n);   // overwrites n samples (any n)
    int  activeVoices() const;                      // Bloom's and Air's voices in use, and Ground if it sounds

    Info info() const;
    const Harmony& harmony() const { return harmony_; }
    const Ground& ground() const { return ground_; }
    const Bloom& bloom() const { return bloom_; }
    const Air& air() const { return air_; }
    const Weather& weather() const { return weather_; }
    const Echo& echo() const { return echo_; }
    Memory& memory() { return memory_; }   // Keep pins it from the loader's thread (dsp/memory.h)
    const Memory& memory() const { return memory_; }

    // Test hooks: the next render() finds a NaN in the reverb's return, or in Echo's (which it then
    // runs); the sample count jumps; Air's strikes and Weather's grains seen as they start.
    void testInjectNaN() { poison_ = true; }
    void testInjectEchoNaN() { poisonEcho_ = true; }
    void testSetSamples(uint64_t n) { samples_ = n; }
    void testAirHook(Air::StrikeHook hook, void* ctx) { air_.setStrikeHook(hook, ctx); }
    void testWeatherHook(Weather::SpawnHook hook, void* ctx) { weather_.setSpawnHook(hook, ctx); }

private:
    enum KeyState : uint8_t { K_UP, K_DOWN, K_PEDAL, K_HOLD };

    // A gain gliding to its target in a straight line: aim() (at a control step) sets the step a
    // sample when the target has changed; next() and apply() take it a sample at a time, across
    // pieces, and land on it exactly.
    struct Glide {
        float now = 1.0f, target = 1.0f, to = 1.0f, step = 0.0f;
        void aim(int samples);               // to the target over `samples`, if it changed
        float next();
        void apply(float* L, float* R, int n);
        void land();                         // at the target now
        bool still() const { return step == 0.0f; }
    };

    void changed(const Patch& was);          // what a patch change does to what sounds: Hold, Listen, On Stop
    void playKey(int key, bool again);       // Bloom (Notes) plays the key's chord
    void releaseLatched(int except);         // the keys Hold latched go
    void makeRoom(bool latchedOut);          // a full harmony lets the oldest pedal or Hold key go
    void letGo(int key);                     // the key's notes released, the key gone from the harmony
    void stop();                             // On Stop
    void route();                            // Ground's target, Bloom's and Air's chords, Weather's gate
    void routeAir();                         // Air's chord and whether it generates, by its Listen
    void routeWeather();                     // Weather's gate and its To Key chord, by its Listen
    void control();                          // a control step
    void clearDsp();                         // every DSP state, silent (reset() and the guard)
    void settle();                           // the output where it settles: glides landed, filters empty
    bool piece(float* outL, float* outR, int n, uint64_t at);   // n <= kChunk samples from `at`; false: not finite
    void applyRemember();                    // a Remember asked for
    double beatsAt(uint64_t at) const;       // the strata's beat count at sample `at`
    void clockStrata(uint64_t at);           // the strata's synced clocks set to it
    bool output(float* L, float* R, int n);   // false: a sample isn't finite
    void tiltFor(float t);                   // the tilt's coefficients for t

    const TableSet& tables_;
    Patch p_;
    Harmony harmony_;
    Ground ground_;
    Bloom bloom_;
    Air air_;
    Weather weather_;
    Echo echo_;
    Memory memory_;
    Space space_;
    Space::Params spaceParams_;              // what Space is given: the patch's, the tail's hold applied
    Transport transport_;
    double echoBpm_ = -1.0;                  // the tempo Echo was last given (it reads nothing else of the transport)
    uint64_t echoTo_ = 0;                    // the sample after the last one Echo ran: rest() for the gap
    const GrainSource* weatherSrc_ = nullptr;   // the plugin's (setWeatherSource)
    bool weatherHolds_ = false;              // Weather's last render was given the plugin's source, ready
    bool rememberAsked_ = false;
    // The strata's one beat count (clockStrata): beats_ at sample beatsAt_ (a block's first). MPC's
    // position while it plays; on from where it was at the tempo while it is stopped.
    double beats_ = 0.0;
    uint64_t beatsAt_ = 0;

    // Keys: down, held by the pedal or latched by Hold; the note Input mapped each to, its
    // velocity, and the chord Bloom played for it (Notes).
    KeyState key_[128] = {};
    int mapped_[128] = {};
    float keyVel_[128] = {};
    uint64_t keyAge_[128] = {};              // the note-on that put each key down: the oldest goes first
    uint64_t keysPressed_ = 0;
    Chord keyChord_[128];
    bool pedal_ = false;
    Chord prev_;                             // the chord Bloom played last (Notes): Leading's start
    float lastVel_ = 0.0f;                   // the last note-on's velocity, 0..1: Harmony and Free moves

    // Routes: what Ground, Bloom, Air and Weather were last given.
    int groundWant_ = -1;
    uint32_t bloomVersion_ = 0;              // the harmony's version Bloom (Harmony) moved to
    bool bloomSync_ = false;                 // Bloom (Harmony) moves to the harmony's chord at the next route
    Chord freeChord_, freePlayed_;           // the tonic chord (Free), and what Bloom was moved to
    Chord airChord_;                         // the chord Air was given, and whether it generates
    bool airGenerate_ = false;
    bool airSync_ = true;                    // Air is given its chord at the next route whatever it was
    bool weatherGate_ = false;               // Weather's gate as the engine set it (Weather::reset() closes it)
    uint16_t weatherPcs_ = 0;                // the To Key chord Weather was given
    bool keysDirty_ = true;                  // a key went down or up since heldPcs_ was worked out
    uint16_t heldPcs_ = 0;                   // the pitch classes of the notes the keys held map to
    bool anyHeld_ = false;                   // a key held (a finger, the pedal or Hold)

    bool awake_ = false;
    bool idle_ = true;
    bool suspended_ = false;
    bool fading_ = false;
    float fadeDb_ = 0.0f;
    Glide fade_;                             // Stop's fade as a gain
    uint64_t samples_ = 0;
    uint32_t guards_ = 0;
    bool poison_ = false, poisonEcho_ = false;
    uint64_t echoRuns_ = 0;

    // The mix and the output.
    float groundSend_ = 0.0f, bloomSend_ = 0.0f, airSend_ = 0.0f, weatherSend_ = 0.0f;   // into Space
    Glide ret_;                              // the Space return
    Glide gPanL_, gPanR_, bPanL_, bPanR_, aPanL_, aPanR_, wPanL_, wPanR_;   // the strata's pans
    Glide gEcho_, bEcho_, aEcho_, wEcho_;    // the strata's Echo sends
    Glide echoRet_, echoSpace_;              // Echo's return into the mix, and its send into Space
    Glide volume_;                           // the volume as a gain
    float tilt_ = 0.0f;                      // where the tilt glides, from tiltNow_
    float tiltNow_ = 0.0f;
    float tiltG_ = 0.0f, tiltLow_ = 1.0f, tiltHigh_ = 1.0f;   // the one-pole's G, the shelf's gains
    float tiltS_[2] = {};                    // the one-pole's state, L and R
    float limitD_ = 0.0f;                    // the limiter's gain under 1 (1 - gain)
    bool limiting_ = false;

    // A piece's buffers: each stratum's dry when it is wanted apart (panned, sent to Echo, Bloom's
    // for Duck), the Space sends (and Space's wet in them), the Echo bus (and Echo's wet in it).
    float gL_[kChunk] = {}, gR_[kChunk] = {}, bL_[kChunk] = {}, bR_[kChunk] = {};
    float aL_[kChunk] = {}, aR_[kChunk] = {}, wL_[kChunk] = {}, wR_[kChunk] = {};
    float sendL_[kChunk] = {}, sendR_[kChunk] = {};
    float echoL_[kChunk] = {}, echoR_[kChunk] = {};
    float tapL_[kChunk] = {}, tapR_[kChunk] = {};   // Memory's tap, written once the guard has looked
};

// Process-wide, summed over every instance, for the soak (tools/soak.cpp), which only sees the
// plugin's entry points: the guard's trips (render() calls it zeroed) and the samples the limiter's
// gain computer ran on. Relaxed atomics, one add a piece at most; they wrap, so read differences.
uint32_t guardTrips();
uint32_t limitedSamples();

} // namespace af
