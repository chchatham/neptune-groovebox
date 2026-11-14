#pragma once

#include "ScreenManager.h"
#include "ParamGrid.h"
#include "ScreenLayout.h"
#include "UiClock.h"
#include "TurnNotice.h"
#include "../controller/DisplayRenderer.h"
#include <functional>
#include <vector>

// -----------------------------------------------------------------------------
// The screen audit: goes through every screen and every page, turns every knob
// one click up and one click back, and checks what the display does.
//
// Everything is done the way the controller does it - the same messages into
// ScreenManager (knob turns, bank buttons, a held pad) - so what is audited is
// what the hardware path really draws. For every picture it checks:
//
//   OVERLAP    text drawn over something else, or something drawn over text (the
//              drawing audit in DisplayRenderer records every such pixel)
//   CLIPPED    text that runs off the edge of the glass
//   DEAD       a labelled knob that changes nothing on the display, not even in the readout
//              just after the turn, for one click or ten, in either direction. A knob that has
//              nothing to act on and says why (TurnNotice: "SONG IS EMPTY" in the name bar for a
//              second) has answered: it counts as SAID, not DEAD, provided the text really is
//              in the name bar just after the turn and gone again once the display has settled
//   SMALL      (for review) one click changes nothing visible but ten do
//   UNLABELLED a knob without a label whose turn does change the display
//   STUCK      one click up and one click back does not give the original picture again
//              (something stayed on the display, or the value did not come back)
//   SPILL      on grid screens: a turn that changes pixels outside the knob's own cell and the
//              name bar once the display has settled (often intended - SYNC renames RATE - so
//              these are listed for review, not counted as faults)
//   PAGING     the bank buttons do not go once round all the pages and back to the first
//   DEVICE     (controller connected) the picture last sent to the device is not the picture
//              the screen drew, a second after the last change
//
// Help cards: on every screen (and with a step held, which changes the EDIT card and the > and
// Up / Down cards) every hold key that has cards - Shift, Func, >, Up, Record - is held past the
// help delay and the bank buttons go through that key's cards. Every card gets the OVERLAP and
// CLIPPED checks; a card that does not appear counts as DEAD, cards that do not go once round as
// PAGING, and a screen that is not back exactly as it was once the key is let go (the cards, or
// the bank buttons under them, changed something) as STUCK. A key whose release acts even after
// its card (Record: recording on / off) is tapped once more before that comparison. The knob
// passes never hold such a key, so no card can appear in them.
//
// "Settled" pictures are taken by moving the screens' clock (UiClock) three seconds on, so the
// knob readout, the label highlight and the filter / envelope picture have gone.
//
// Output in the folder given: report.txt (one line per finding, then totals), one PBM per page
// (settled) in pages/, one per help card in help/, and for every finding the picture that shows it
// in findings/.
// With `photoHandshake` the audit also stops on every page until <folder>/shots/<n>.done appears,
// so a script can photograph the device showing exactly that page (tools/screen_audit.py).
//
// run() blocks; call it from a thread of its own (it takes the screen manager's lock around
// every action, like the MIDI thread does).
// -----------------------------------------------------------------------------
class ScreenAudit
{
public:
    struct Target
    {
        int screenIndex = 0;
        juce::String name;
        bool perEngine = false;     // repeat for every engine on the selected track
        bool withHeldStep = false;  // repeat once more with a step pad held (parameter locks)
    };

    // (The audit holds every help key right after turning knobs: the "in the middle of something"
    // wait is switched off while it runs.)
    ScreenAudit(ScreenManager& sm, DisplayRenderer& d) : screens(sm), display(d) { screens.setHelpBusyWindow(0); }
    ~ScreenAudit() { screens.setHelpBusyWindow(ScreenManager::HELP_BUSY_WINDOW_MS); }

    std::vector<Target> targets;
    juce::StringArray engineNames;                       // one per engine type, in enum order
    std::function<void(int engineIndex)> setEngine;      // the selected track's engine
    bool deviceConnected = false;
    bool photoHandshake = false;
    int photoTimeoutMs = 20000;
    std::function<void(const juce::String&)> log = [](const juce::String&) {};

    struct Totals
    {
        int pages = 0, turns = 0, pictures = 0, helpCards = 0;
        int overlap = 0, clipped = 0, dead = 0, unlabelled = 0, stuck = 0, spill = 0, paging = 0, device = 0, small = 0;
        int said = 0;       // knobs that had nothing to act on and said why
        double worstRenderMs = 0.0;
        int faults() const { return overlap + clipped + dead + unlabelled + stuck + paging + device; }
    };

    Totals run(const juce::File& folder)
    {
        dir = folder;
        dir.createDirectory();
        dir.getChildFile("pages").deleteRecursively();
        dir.getChildFile("findings").deleteRecursively();
        dir.getChildFile("shots").deleteRecursively();
        dir.getChildFile("help").deleteRecursively();
        dir.getChildFile("pages").createDirectory();
        dir.getChildFile("help").createDirectory();
        dir.getChildFile("findings").createDirectory();
        if (photoHandshake) dir.getChildFile("shots").createDirectory();
        lines.clear();
        totals = {};
        serial = 0;

        display.setDrawAudit(&drawAudit);
        const int framesBefore = display.getFramesSent(), timeoutsBefore = display.getTimeoutCount();

        for (auto& target : targets)
        {
            const int engines = (target.perEngine && setEngine) ? engineNames.size() : 1;
            for (int e = 0; e < engines; ++e)
            {
                juce::String label = target.name;
                if (target.perEngine && setEngine)
                {
                    { const juce::ScopedLock ui(screens.getLock()); setEngine(e); }
                    label << " [" << engineNames[e] << "]";
                }
                auditScreen(target, label, false);
                if (target.withHeldStep && e == 0)
                    auditScreen(target, label + " +step", true);
            }
            // The other screens are audited on the default engine.
            if (target.perEngine && setEngine) { const juce::ScopedLock ui(screens.getLock()); setEngine(0); }
        }
        if (setEngine) { const juce::ScopedLock ui(screens.getLock()); setEngine(0); }

        display.setDrawAudit(nullptr);

        juce::String report;
        report << "Screen audit, " << juce::Time::getCurrentTime().toString(true, true) << "\n"
               << (deviceConnected ? "Controller connected: pictures were sent to the device and checked.\n"
                                   : "No controller: drawing checks only.\n") << "\n";
        for (auto& l : lines) report << l << "\n";
        report << "\n" << totals.pages << " pages, " << totals.turns << " knob turns, " << totals.helpCards << " help cards, "
               << totals.pictures << " pictures checked\n"
               << "OVERLAP " << totals.overlap << "   CLIPPED " << totals.clipped << "   DEAD " << totals.dead
               << "   UNLABELLED " << totals.unlabelled << "   STUCK " << totals.stuck << "   PAGING " << totals.paging
               << "   DEVICE " << totals.device << "   (for review: SPILL " << totals.spill << ", SMALL " << totals.small
               << ", SAID " << totals.said << ")\n"
               << "Slowest picture took " << juce::String(totals.worstRenderMs, 2) << " ms to draw\n";
        if (deviceConnected)
            report << "Frames sent to the device " << (display.getFramesSent() - framesBefore)
                   << ", mean time to its ready reply " << juce::String(display.getMeanAckMs(), 1)
                   << " ms, replies that never came " << (display.getTimeoutCount() - timeoutsBefore) << "\n";
        dir.getChildFile("report.txt").replaceWithText(report);
        log(report.fromLastOccurrenceOf("\n\n", false, false));
        return totals;
    }

private:
    using Pixels = DisplayRenderer::PixelBuffer;
    struct Picture
    {
        Pixels px {};
        DisplayRenderer::DrawAudit::Box overText, clipped;
    };
    struct Box
    {
        int x0 = 128, y0 = 64, x1 = -1, y1 = -1, count = 0;
        void add(int x, int y) { x0 = juce::jmin(x0, x); y0 = juce::jmin(y0, y); x1 = juce::jmax(x1, x); y1 = juce::jmax(y1, y); ++count; }
        juce::String text() const
        {
            return "x " + juce::String(x0) + "-" + juce::String(x1) + ", y " + juce::String(y0) + "-" + juce::String(y1)
                 + " (" + juce::String(count) + " px)";
        }
    };
    template <typename B> static juce::String boxText(const B& b)
    {
        return "x " + juce::String(b.x0) + "-" + juce::String(b.x1) + ", y " + juce::String(b.y0) + "-" + juce::String(b.y1)
             + " (" + juce::String(b.count) + " px)";
    }

    // ---- Doing what the controller does ----------------------------------------------------------

    void turn(int encoder, int clicks)
    {
        const juce::ScopedLock ui(screens.getLock());
        screens.controlChangeReceived(16, 0x15 + encoder, clicks > 0 ? clicks : 128 + clicks);
        ++totals.turns;
    }

    void bank(bool down)
    {
        const juce::ScopedLock ui(screens.getLock());
        const int cc = down ? ScreenManager::CC_BANK_DOWN : ScreenManager::CC_BANK_UP;
        screens.controlChangeReceived(ScreenManager::BUTTON_CHANNEL, cc, 127);
        screens.controlChangeReceived(ScreenManager::BUTTON_CHANNEL, cc, 0);
    }

    void pad(int index, bool down)
    {
        const juce::ScopedLock ui(screens.getLock());
        const int note = index < 8 ? 96 + index : 112 + (index - 8);
        if (down) screens.noteOnReceived(1, note, 100);
        else      screens.noteOffReceived(1, note);
    }

    // A hold key that has help cards, as the controller sends it (Up stands for Up / Down).
    void holdKey(ScreenManager::HelpKey key, bool down)
    {
        using Key = ScreenManager::HelpKey;
        const juce::ScopedLock ui(screens.getLock());
        const int value = down ? 127 : 0;
        switch (key)
        {
            case Key::Shift:   screens.controlChangeReceived(ScreenManager::SHIFT_CHANNEL, ScreenManager::CC_SHIFT, value); break;
            case Key::Func:    screens.controlChangeReceived(ScreenManager::BUTTON_CHANNEL, ScreenManager::CC_FUNC, value); break;
            case Key::Pattern: screens.controlChangeReceived(ScreenManager::BUTTON_CHANNEL, ScreenManager::CC_PATTERN_KEY, value); break;
            case Key::Track:   screens.controlChangeReceived(ScreenManager::BUTTON_CHANNEL, ScreenManager::CC_ARROW_UP, value); break;
            case Key::Record:  screens.controlChangeReceived(ScreenManager::BUTTON_CHANNEL, ScreenManager::CC_RECORD, value); break;
            case Key::None:
            case Key::NumKeys: break;
        }
    }

    // The picture as it is right now (just after a turn: with the readout, highlight, pictures).
    Picture snap()
    {
        const juce::ScopedLock ui(screens.getLock());
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        screens.requestRedraw();
        screens.redrawIfNeeded();
        totals.worstRenderMs = juce::jmax(totals.worstRenderMs, juce::Time::getMillisecondCounterHiRes() - t0);
        ++totals.pictures;
        Picture p;
        p.px = display.getPixels();
        p.overText = drawAudit.overText;
        p.clipped = drawAudit.clipped;
        return p;
    }

    // The picture once everything momentary has gone.
    Picture settled()
    {
        UiClock::advance(3000);
        return snap();
    }

    static bool same(const Pixels& a, const Pixels& b) { return a == b; }

    static Box difference(const Pixels& a, const Pixels& b)
    {
        Box box;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 128; ++x)
                if (a[(size_t) y][(size_t) x] != b[(size_t) y][(size_t) x])
                    box.add(x, y);
        return box;
    }

    // ---- Recording -------------------------------------------------------------------------------

    static juce::String pbm(const Pixels& px)
    {
        juce::String s = "P1\n128 64\n";
        for (int y = 0; y < 64; ++y)
        {
            for (int x = 0; x < 128; ++x)
                s << (px[(size_t) y][(size_t) x] ? "1 " : "0 ");
            s << "\n";
        }
        return s;
    }

    static juce::String fileSafe(juce::String s)
    {
        return s.replaceCharacters("/\\:*?\"<>|", "---------").trim();
    }

    void finding(const juce::String& kind, const juce::String& where, const juce::String& what, const Pixels* picture)
    {
        juce::String line = kind.paddedRight(' ', 11) + where + ": " + what;
        if (picture != nullptr)
        {
            const juce::String name = juce::String(serial++).paddedLeft('0', 4) + " " + kind + " " + fileSafe(where) + ".pbm";
            dir.getChildFile("findings").getChildFile(name).replaceWithText(pbm(*picture));
            line << "   [findings/" << name << "]";
        }
        lines.add(line);
    }

    // OVERLAP and CLIPPED for one picture. Each distinct place is reported once per page.
    void checkDrawing(const Picture& p, const juce::String& where, juce::StringArray& seenOnPage)
    {
        if (p.overText.any())
        {
            const juce::String key = "O" + boxText(p.overText);
            if (!seenOnPage.contains(key))
            {
                seenOnPage.add(key);
                ++totals.overlap;
                finding("OVERLAP", where, "text and other drawing share pixels at " + boxText(p.overText), &p.px);
            }
        }
        if (p.clipped.any())
        {
            const juce::String key = "C" + boxText(p.clipped);
            if (!seenOnPage.contains(key))
            {
                seenOnPage.add(key);
                ++totals.clipped;
                finding("CLIPPED", where, "text runs off the glass at " + boxText(p.clipped), &p.px);
            }
        }
    }

    // DEVICE: a moment after the last change the device must have been sent this very picture.
    void checkDevice(const Pixels& expected, const juce::String& where)
    {
        if (!deviceConnected)
            return;
        // (The refresh timer hands the display the same picture thirty times a second, so "idle"
        // is only asked once it has been seen: asking again a moment later can catch a hand-over.)
        bool idle = display.isIdle();
        for (int waited = 0; waited < 1500 && !idle; waited += 5)
        {
            juce::Thread::sleep(5);
            idle = display.isIdle();
        }
        Pixels sent;
        display.getLastSentPixels(sent);
        if (!idle)
        {
            ++totals.device;
            finding("DEVICE", where, juce::String("the display did not come to rest within 1.5 s (")
                                     + (display.isWaitingForAck() ? "no ready reply from the device" : "a picture was left waiting to go out") + ")", nullptr);
        }
        else if (!same(sent, expected))
        {
            // The refresh timer may have drawn since (a blink, a meter): compare with the picture now.
            const auto now = snap();
            for (int waited = 0; waited < 1500 && !display.isIdle(); waited += 5)
                juce::Thread::sleep(5);
            display.getLastSentPixels(sent);
            if (!same(sent, now.px))
            {
                ++totals.device;
                finding("DEVICE", where, "the device was sent a different picture: " + difference(sent, now.px).text(), &sent);
            }
        }
    }

    void photo(const Pixels& expected, const juce::String& where)
    {
        if (!photoHandshake)
            return;
        const juce::String stem = juce::String(totals.pages).paddedLeft('0', 4);
        auto shots = dir.getChildFile("shots");
        shots.getChildFile(stem + ".pbm").replaceWithText(pbm(expected));
        shots.getChildFile(stem + ".txt").replaceWithText(where);
        auto done = shots.getChildFile(stem + ".done");
        for (int waited = 0; waited < photoTimeoutMs && !done.existsAsFile(); waited += 20)
            juce::Thread::sleep(20);
    }

    // ---- One screen ------------------------------------------------------------------------------

    void auditScreen(const Target& target, const juce::String& label, bool heldStep)
    {
        Screen* screen = nullptr;
        {
            const juce::ScopedLock ui(screens.getLock());
            screens.setActiveScreen(target.screenIndex == 0 ? 1 : 0);     // leave and come back: a fresh visit
            screens.setActiveScreen(target.screenIndex);
            screen = screens.getActiveScreen();
        }
        if (screen == nullptr)
            return;

        if (heldStep)
        {
            pad(0, true);
            UiClock::advance(1000);     // a hold, not a tap: STEP shows the step's pages by itself
        }

        auto pageCount = [&] { const juce::ScopedLock ui(screens.getLock()); return juce::jmax(1, screen->getPageCount()); };
        auto pageIndex = [&] { const juce::ScopedLock ui(screens.getLock()); return screen->getPageIndex(); };

        // PAGING: the lower bank button goes through every page once and comes back to the first;
        // the upper one goes straight from the first to the last.
        const int n = pageCount();
        for (int i = 0; i < n && pageIndex() != 0; ++i)
            bank(false);
        if (n > 1)
        {
            juce::String visited;
            bool ok = pageIndex() == 0;
            for (int i = 0; i < n; ++i)
            {
                bank(true);
                visited << (pageIndex() + 1) << " ";
                ok = ok && pageIndex() == (i + 1) % n;
            }
            bank(false);
            ok = ok && pageIndex() == n - 1;
            bank(true);
            ok = ok && pageIndex() == 0;
            if (!ok)
            {
                ++totals.paging;
                finding("PAGING", label, "bank-down from page 1 of " + juce::String(n) + " visits " + visited.trim()
                                         + "; bank-up from page 1 must give the last page", nullptr);
            }
            for (int i = 0; i < n && pageIndex() != 0; ++i)
                bank(false);
        }

        for (int page = 0; page < pageCount(); ++page)
        {
            auditPage(*screen, label + " " + juce::String(page + 1) + " of " + juce::String(pageCount()), heldStep);
            bank(true);
        }

        auditHelpCards(label);

        if (heldStep)
            pad(0, false);
    }

    // ---- The help cards of one screen, key after key, as it stands (a step may be held) -----------

    void auditHelpCards(const juce::String& label)
    {
        using Key = ScreenManager::HelpKey;
        juce::StringArray seen;
        const Picture before = settled();

        for (int k = 0; k < (int) Key::NumKeys; ++k)
        {
            const Key key = (Key) k;
            const juce::String keyName = ScreenManager::getHelpKeyName(key);
            holdKey(key, true);
            UiClock::advance(ScreenManager::HELP_DELAY_MS + 50);

            auto showing = [&] { const juce::ScopedLock ui(screens.getLock()); return screens.getHelpCardKey() == key; };
            auto cardPage = [&] { const juce::ScopedLock ui(screens.getLock()); return screens.getHelpCardPage(key); };
            int count = 0;
            { const juce::ScopedLock ui(screens.getLock()); count = screens.getHelpCardCount(key); }

            if (!showing())
            {
                ++totals.dead;
                finding("DEAD", label + ", " + keyName + " help", "no help card after the key was held past the delay", nullptr);
                holdKey(key, false);
                if (ScreenManager::helpKeyActsOnReleaseAfterCard(key)) { holdKey(key, true); holdKey(key, false); }
                continue;
            }

            // Back to the first card (the upper bank button), then once round with the lower one.
            for (int i = 0; i < count && cardPage() != 0; ++i)
                bank(false);
            bool round = cardPage() == 0;
            for (int i = 0; i < count; ++i)
            {
                const juce::String where = label + ", " + keyName + " help " + juce::String(i + 1) + " of " + juce::String(count);
                const Picture card = snap();
                ++totals.helpCards;
                dir.getChildFile("help").getChildFile(juce::String(totals.helpCards).paddedLeft('0', 4) + " " + fileSafe(where) + ".pbm")
                   .replaceWithText(pbm(card.px));
                checkDrawing(card, where, seen);
                if (same(card.px, before.px))
                {
                    ++totals.dead;
                    finding("DEAD", where, "the card did not replace the screen's picture", &card.px);
                }
                checkDevice(card.px, where);
                bank(true);
                round = round && showing() && cardPage() == (i + 1) % count;
            }
            if (!round)
            {
                ++totals.paging;
                finding("PAGING", label + ", " + keyName + " help", "the bank buttons do not go once round the "
                                                                    + juce::String(count) + " cards", nullptr);
            }

            holdKey(key, false);
            // Record let go switched recording on (or off): the same again puts it back.
            if (ScreenManager::helpKeyActsOnReleaseAfterCard(key)) { holdKey(key, true); holdKey(key, false); }
            const Picture after = settled();
            if (!same(after.px, before.px))
            {
                ++totals.stuck;
                finding("STUCK", label + ", " + keyName + " help", "after the key is let go the screen differs at "
                                                                   + difference(after.px, before.px).text(), &after.px);
            }
        }
    }

    void auditPage(Screen& screen, const juce::String& where, bool heldStep)
    {
        juce::StringArray seen;
        const Picture base = settled();
        ++totals.pages;

        juce::String sub;
        Screen::EncoderLayout layout;
        juce::StringArray names;
        {
            const juce::ScopedLock ui(screens.getLock());
            sub = screens.composeNameBar().text;
            layout = screen.getEncoderLayout();
            for (int e = 0; e < 8; ++e)
                names.add(screen.getEncoderName(e));
        }
        const juce::String pageWhere = where + (sub.isNotEmpty() ? " (" + sub + ")" : juce::String());

        dir.getChildFile("pages").getChildFile(juce::String(totals.pages).paddedLeft('0', 4) + " " + fileSafe(pageWhere) + ".pbm")
           .replaceWithText(pbm(base.px));
        checkDrawing(base, pageWhere, seen);
        checkDevice(base.px, pageWhere);
        photo(base.px, pageWhere);

        Picture cur = base;
        for (int e = 0; e < 8; ++e)
        {
            const juce::String name = names[e];
            const bool labelled = name.isNotEmpty() && name != "---" && !name.containsOnly("- ");
            const juce::String knob = pageWhere + ", knob " + juce::String(e + 1) + (labelled ? " " + name : juce::String(" (no label)"));

            // One click up and one click back. Three pictures decide what happened:
            //   up differs from before            the knob works; back must equal before
            //   up equal, back differs            the knob was at its top: one more click up restores it
            //   both equal                        nothing shows once settled. If the pictures taken
            //                                     just after the two turns differ (the readout), the
            //                                     value is only shown while turning - normal on the bar
            //                                     screens. Otherwise try ten clicks before calling it dead.
            juce::String barBefore;
            { const juce::ScopedLock ui(screens.getLock()); barBefore = screens.composeNameBar().text; }
            const int noticesBefore = TurnNotice::count();
            turn(e, 1);
            const Picture upNow = snap();
            // A notice: said at this turn, and its text is what the name bar shows now (the
            // picture differs from the settled one in the name bar's rows).
            juce::String barNow;
            { const juce::ScopedLock ui(screens.getLock()); barNow = screens.composeNameBar().text; }
            bool saidWhy = TurnNotice::count() != noticesBefore && barNow.isNotEmpty() && barNow != barBefore;
            if (saidWhy)
            {
                bool barChanged = false;
                for (int y = 0; y < 10 && !barChanged; ++y)
                    for (int x = 0; x < 128 && !barChanged; ++x)
                        barChanged = upNow.px[(size_t) y][(size_t) x] != cur.px[(size_t) y][(size_t) x];
                saidWhy = barChanged;
            }
            const Picture up = settled();
            checkDrawing(upNow, knob + ", just turned", seen);
            checkDrawing(up, knob + ", turned", seen);
            turn(e, -1);
            const Picture downNow = snap();
            Picture back = settled();
            checkDrawing(downNow, knob + ", just turned back", seen);

            Picture moved = up;
            bool responds = !same(up.px, cur.px);
            bool small = false;
            if (!responds && !same(back.px, cur.px))
            {
                responds = true;
                moved = back;
                turn(e, 1);
                back = settled();
            }
            else if (!responds && !same(upNow.px, downNow.px))
            {
                responds = true;            // shown in the readout only
            }
            else if (!responds && saidWhy)
            {
                responds = true;            // nothing to act on, and the name bar said why
                ++totals.said;
                finding("SAID", knob, "\"" + barNow + "\"", nullptr);
            }
            else if (!responds)
            {
                turn(e, 10);
                moved = settled();
                turn(e, -10);
                back = settled();
                if (same(moved.px, cur.px) && !same(back.px, cur.px))
                {
                    moved = back;
                    turn(e, 10);
                    back = settled();
                }
                small = !same(moved.px, cur.px);
                responds = small;
            }
            if (small && labelled)
            {
                ++totals.small;
                finding("SMALL", knob, "one click changes nothing on the display; ten clicks do", &cur.px);
            }

            if (labelled && !responds)
            {
                ++totals.dead;
                finding("DEAD", knob, "neither one click nor ten, either way, change anything on the display", &cur.px);
            }
            if (!labelled && responds)
            {
                ++totals.unlabelled;
                finding("UNLABELLED", knob, "the display changes at " + difference(moved.px, cur.px).text(), &moved.px);
            }
            // With a step held a turn makes a parameter lock, and its mark rightly stays.
            if (responds && !heldStep && !same(back.px, cur.px))
            {
                ++totals.stuck;
                finding("STUCK", knob, "after one click and one click back the display differs at "
                                       + difference(back.px, cur.px).text(), &back.px);
            }
            if (responds && layout == Screen::EncoderLayout::Grid)
            {
                // Outside the knob's own cell and the name bar.
                Box outside;
                const int cx = ParamGrid::cellX(e), cy = ParamGrid::cellY(e);
                for (int y = 11; y < 64; ++y)
                    for (int x = 0; x < 128; ++x)
                        if (moved.px[(size_t) y][(size_t) x] != cur.px[(size_t) y][(size_t) x]
                            && !(x >= cx && x < cx + ParamGrid::CELL_W && y >= cy - 1 && y < cy + 21))
                            outside.add(x, y);
                if (outside.count > 0)
                {
                    ++totals.spill;
                    finding("SPILL", knob, "also changes " + outside.text(), &moved.px);
                }
            }
            checkDevice(back.px, knob);

            // The next knob starts from the picture as it now is, so one knob that does not come
            // back is reported once and not again for every knob after it.
            cur = back;
        }
        juce::ignoreUnused(heldStep);
    }

    ScreenManager& screens;
    DisplayRenderer& display;
    DisplayRenderer::DrawAudit drawAudit;
    juce::File dir;
    juce::StringArray lines;
    Totals totals;
    int serial = 0;
};
