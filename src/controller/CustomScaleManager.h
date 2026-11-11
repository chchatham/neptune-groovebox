#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <string>
#include <functional>

// Data model for a single scale page
struct ScalePage
{
    std::string name;
    int rootNote = 60;
    std::array<int, 16> padNotes {};
    bool isFactory = false;

    // Serialization using JUCE var system
    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("name", juce::String(name));
        obj->setProperty("rootNote", rootNote);
        obj->setProperty("isFactory", isFactory);

        juce::Array<juce::var> notesArray;
        for (int n : padNotes)
            notesArray.add(n);
        obj->setProperty("padNotes", notesArray);

        return juce::var(obj);
    }

    static ScalePage fromVar(const juce::var& v)
    {
        ScalePage page;
        if (auto* obj = v.getDynamicObject())
        {
            page.name = obj->getProperty("name").toString().toStdString();
            page.rootNote = (int) obj->getProperty("rootNote");
            page.isFactory = (bool) obj->getProperty("isFactory");

            if (auto* arr = obj->getProperty("padNotes").getArray())
            {
                for (int i = 0; i < juce::jmin((int) arr->size(), 16); ++i)
                    page.padNotes[static_cast<size_t>(i)] = (int) (*arr)[i];
            }
        }
        return page;
    }
};

// Per-step page change lock
struct PageChangeLock
{
    int pageIndex = -1;  // -1 = no lock
};

class CustomScaleManager
{
public:
    static constexpr int MAX_PAGES = 32;
    static constexpr int PADS_PER_PAGE = 16;
    static constexpr int DEFAULT_PAGE_COUNT = 12;

    enum class Mode
    {
        Relative,   // notes are offsets from rootNote
        Absolute    // notes are fixed MIDI notes
    };

    // Legacy Page struct kept for internal storage; mirrors ScalePage
    struct Page
    {
        const char* name = "Empty";
        std::string nameStorage;       // owns the string when not factory
        int rootNote = 60;
        Mode mode = Mode::Relative;
        int notes[PADS_PER_PAGE] = {};
        bool isFactory = false;

        // Get name, preferring nameStorage if non-empty
        const char* getName() const
        {
            return nameStorage.empty() ? name : nameStorage.c_str();
        }

        void setName(const std::string& n)
        {
            nameStorage = n;
            name = nameStorage.c_str();
        }
    };

    // Keyboard page selection mode
    enum class KeySelectMode
    {
        Off,
        Latched,     // stays on selected page
        Momentary    // returns when key released
    };

    // Callback for transposition when page changes
    using TransposeCallback = std::function<void(int semitones)>;

    CustomScaleManager()
    {
        initDefaultPages();
    }

    int getPageCount() const { return pageCount; }

    // Add a new empty page, returns its index or -1 if at max
    int addPage(const std::string& pageName = "User")
    {
        if (pageCount >= MAX_PAGES)
            return -1;

        auto& p = pages[static_cast<size_t>(pageCount)];
        p.setName(pageName);
        p.rootNote = 60;
        p.mode = Mode::Relative;
        p.isFactory = false;
        for (int i = 0; i < PADS_PER_PAGE; ++i)
            p.notes[i] = i; // chromatic by default

        return pageCount++;
    }

    // Remove a non-factory page by index
    bool removePage(int index)
    {
        if (index < 0 || index >= pageCount)
            return false;
        if (pages[static_cast<size_t>(index)].isFactory)
            return false;

        // Shift pages down
        for (int i = index; i < pageCount - 1; ++i)
            pages[static_cast<size_t>(i)] = pages[static_cast<size_t>(i + 1)];

        --pageCount;
        if (activePage >= pageCount)
            activePage = pageCount - 1;
        if (activePage < 0)
            activePage = 0;

        return true;
    }

    void setActivePage(int index)
    {
        int newPage = juce::jlimit(0, juce::jmax(0, pageCount - 1), index);

        if (newPage != activePage)
        {
            int oldRoot = pages[static_cast<size_t>(activePage)].rootNote;
            int prevPage = activePage;
            activePage = newPage;
            int newRoot = pages[static_cast<size_t>(activePage)].rootNote;

            // Notify transpose if roots differ and callback is set
            int interval = newRoot - oldRoot;
            if (interval != 0 && transposeCallback)
                transposeCallback(interval);

            juce::ignoreUnused(prevPage);
        }
    }

    int getActivePage() const { return activePage; }

    int getNoteForPad(int padIndex) const
    {
        if (padIndex < 0 || padIndex >= PADS_PER_PAGE)
            return -1;

        auto& page = pages[static_cast<size_t>(activePage)];

        if (page.mode == Mode::Absolute)
            return juce::jlimit(0, 127, page.notes[padIndex]);

        return juce::jlimit(0, 127, page.rootNote + page.notes[padIndex]);
    }

    // Get note for a specific page (for page-change locks)
    int getNoteForPadOnPage(int pageIndex, int padIndex) const
    {
        if (pageIndex < 0 || pageIndex >= pageCount) return -1;
        if (padIndex < 0 || padIndex >= PADS_PER_PAGE) return -1;

        auto& page = pages[static_cast<size_t>(pageIndex)];
        if (page.mode == Mode::Absolute)
            return juce::jlimit(0, 127, page.notes[padIndex]);

        return juce::jlimit(0, 127, page.rootNote + page.notes[padIndex]);
    }

    void setNoteForPad(int pageIndex, int padIndex, int note)
    {
        if (pageIndex < 0 || pageIndex >= pageCount) return;
        if (padIndex < 0 || padIndex >= PADS_PER_PAGE) return;

        pages[static_cast<size_t>(pageIndex)].notes[padIndex] = note;
    }

    void setRootNote(int pageIndex, int note)
    {
        if (pageIndex < 0 || pageIndex >= pageCount) return;
        pages[static_cast<size_t>(pageIndex)].rootNote = juce::jlimit(0, 127, note);
    }

    int getRootNote() const
    {
        return pages[static_cast<size_t>(activePage)].rootNote;
    }

    int getRootNote(int pageIndex) const
    {
        if (pageIndex < 0 || pageIndex >= pageCount) return 60;
        return pages[static_cast<size_t>(pageIndex)].rootNote;
    }

    const char* getPageName(int index) const
    {
        if (index < 0 || index >= pageCount)
            return "";
        return pages[static_cast<size_t>(index)].getName();
    }

    void setPageName(int index, const std::string& name)
    {
        if (index < 0 || index >= pageCount) return;
        pages[static_cast<size_t>(index)].setName(name);
    }

    Mode getPageMode(int index) const
    {
        if (index < 0 || index >= pageCount)
            return Mode::Relative;
        return pages[static_cast<size_t>(index)].mode;
    }

    void setPageMode(int pageIndex, Mode mode)
    {
        if (pageIndex < 0 || pageIndex >= pageCount) return;
        pages[static_cast<size_t>(pageIndex)].mode = mode;
    }

    bool isPageFactory(int index) const
    {
        if (index < 0 || index >= pageCount) return false;
        return pages[static_cast<size_t>(index)].isFactory;
    }

    // Direct page access for tests
    Page& getPage(int index) { return pages[static_cast<size_t>(index)]; }
    const Page& getPage(int index) const { return pages[static_cast<size_t>(index)]; }

    // --- Keyboard page selection ---

    void setKeySelectMode(KeySelectMode m) { keySelectMode = m; }
    KeySelectMode getKeySelectMode() const { return keySelectMode; }

    // Called when a keyboard key is pressed: selects a page based on note
    // Returns the page index selected, or -1 if no mapping
    int onKeyboardNoteForPageSelect(int midiNote)
    {
        if (keySelectMode == KeySelectMode::Off)
            return -1;

        // Map by root note: find a page whose root matches this note's pitch class
        // If keyByPageNumber is true, use note offset from C3 as page index
        int pageIdx = -1;

        if (keyByPageNumber)
        {
            // Notes starting from C3 (60) map to page 0, 1, 2, ...
            pageIdx = midiNote - 60;
        }
        else
        {
            // Find first page whose root note matches this pitch class
            int pitchClass = midiNote % 12;
            for (int i = 0; i < pageCount; ++i)
            {
                if (pages[static_cast<size_t>(i)].rootNote % 12 == pitchClass)
                {
                    pageIdx = i;
                    break;
                }
            }
        }

        if (pageIdx < 0 || pageIdx >= pageCount)
            return -1;

        if (keySelectMode == KeySelectMode::Momentary)
            pageBeforeMomentary = activePage;

        setActivePage(pageIdx);
        return pageIdx;
    }

    // Called when key released (for momentary mode)
    void onKeyboardNoteReleasedForPageSelect(int midiNote)
    {
        if (keySelectMode != KeySelectMode::Momentary)
            return;

        // Check if this note corresponds to the current page
        int expectedPage = -1;
        if (keyByPageNumber)
            expectedPage = midiNote - 60;
        else
        {
            int pitchClass = midiNote % 12;
            for (int i = 0; i < pageCount; ++i)
            {
                if (pages[static_cast<size_t>(i)].rootNote % 12 == pitchClass)
                {
                    expectedPage = i;
                    break;
                }
            }
        }

        if (expectedPage == activePage && pageBeforeMomentary >= 0)
        {
            // Restore the previous page (use direct assignment to avoid transpose)
            activePage = juce::jlimit(0, pageCount - 1, pageBeforeMomentary);
            pageBeforeMomentary = -1;
        }
    }

    void setKeyByPageNumber(bool byNumber) { keyByPageNumber = byNumber; }
    bool getKeyByPageNumber() const { return keyByPageNumber; }

    // --- Edit mode ---

    bool isEditMode() const { return editMode; }

    void setEditMode(bool enabled)
    {
        editMode = enabled;
        if (!enabled)
            editPadIndex = -1;
    }

    int getEditPadIndex() const { return editPadIndex; }

    void setEditPadIndex(int padIndex)
    {
        if (padIndex >= 0 && padIndex < PADS_PER_PAGE)
            editPadIndex = padIndex;
    }

    // Assign a MIDI note to the currently selected pad in edit mode
    void assignNoteToEditPad(int midiNote)
    {
        if (!editMode || editPadIndex < 0 || editPadIndex >= PADS_PER_PAGE)
            return;

        auto& page = pages[static_cast<size_t>(activePage)];
        if (page.mode == Mode::Absolute)
            page.notes[editPadIndex] = juce::jlimit(0, 127, midiNote);
        else
            page.notes[editPadIndex] = midiNote - page.rootNote;
    }

    // Shift all notes on current page by octave
    void shiftPageOctave(int direction)
    {
        auto& page = pages[static_cast<size_t>(activePage)];
        int shift = direction * 12;

        if (page.mode == Mode::Absolute)
        {
            for (int i = 0; i < PADS_PER_PAGE; ++i)
                page.notes[i] = juce::jlimit(0, 127, page.notes[i] + shift);
        }
        else
        {
            page.notes[0] += shift; // only shift base offset; relative notes stay relative
            // Actually shift all offsets
            for (int i = 0; i < PADS_PER_PAGE; ++i)
                page.notes[i] += shift;
        }
    }

    // --- Copy/paste/clear ---

    void copyPage(int sourceIndex)
    {
        if (sourceIndex < 0 || sourceIndex >= pageCount) return;
        clipboard = pages[static_cast<size_t>(sourceIndex)];
        hasClipboard = true;
    }

    bool pastePage(int destIndex)
    {
        if (!hasClipboard) return false;
        if (destIndex < 0 || destIndex >= pageCount) return false;

        pages[static_cast<size_t>(destIndex)] = clipboard;
        pages[static_cast<size_t>(destIndex)].isFactory = false;
        return true;
    }

    void clearPage(int index)
    {
        if (index < 0 || index >= pageCount) return;

        auto& p = pages[static_cast<size_t>(index)];
        p.setName("Empty");
        p.rootNote = 60;
        p.mode = Mode::Relative;
        p.isFactory = false;
        for (int i = 0; i < PADS_PER_PAGE; ++i)
            p.notes[i] = i; // chromatic default
    }

    bool hasClipboardData() const { return hasClipboard; }

    // --- Transpose callback ---

    void setTransposeCallback(TransposeCallback cb) { transposeCallback = std::move(cb); }

    // Compute the transpose interval for a page-change lock
    int getTransposeInterval(int fromPageIndex, int toPageIndex) const
    {
        if (fromPageIndex < 0 || fromPageIndex >= pageCount) return 0;
        if (toPageIndex < 0 || toPageIndex >= pageCount) return 0;

        return pages[static_cast<size_t>(toPageIndex)].rootNote
             - pages[static_cast<size_t>(fromPageIndex)].rootNote;
    }

    // --- Serialization ---

    juce::var toVar() const
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("pageCount", pageCount);
        obj->setProperty("activePage", activePage);

        juce::Array<juce::var> pagesArray;
        for (int i = 0; i < pageCount; ++i)
        {
            auto& p = pages[static_cast<size_t>(i)];
            auto* pageObj = new juce::DynamicObject();
            pageObj->setProperty("name", juce::String(p.getName()));
            pageObj->setProperty("rootNote", p.rootNote);
            pageObj->setProperty("mode", static_cast<int>(p.mode));
            pageObj->setProperty("isFactory", p.isFactory);

            juce::Array<juce::var> notesArr;
            for (int n = 0; n < PADS_PER_PAGE; ++n)
                notesArr.add(p.notes[n]);
            pageObj->setProperty("notes", notesArr);

            pagesArray.add(juce::var(pageObj));
        }
        obj->setProperty("pages", pagesArray);

        return juce::var(obj);
    }

    void fromVar(const juce::var& v)
    {
        if (auto* obj = v.getDynamicObject())
        {
            int count = (int) obj->getProperty("pageCount");
            activePage = (int) obj->getProperty("activePage");

            if (auto* arr = obj->getProperty("pages").getArray())
            {
                pageCount = juce::jmin(count, MAX_PAGES);
                for (int i = 0; i < pageCount && i < (int) arr->size(); ++i)
                {
                    if (auto* pageObj = (*arr)[i].getDynamicObject())
                    {
                        auto& p = pages[static_cast<size_t>(i)];
                        p.setName(pageObj->getProperty("name").toString().toStdString());
                        p.rootNote = (int) pageObj->getProperty("rootNote");
                        p.mode = static_cast<Mode>((int) pageObj->getProperty("mode"));
                        p.isFactory = (bool) pageObj->getProperty("isFactory");

                        if (auto* notes = pageObj->getProperty("notes").getArray())
                        {
                            for (int n = 0; n < PADS_PER_PAGE && n < (int) notes->size(); ++n)
                                p.notes[n] = (int) (*notes)[n];
                        }
                    }
                }
            }

            activePage = juce::jlimit(0, juce::jmax(0, pageCount - 1), activePage);
        }
    }

    // Export a single page as a shareable preset
    juce::var exportPagePreset(int index) const
    {
        if (index < 0 || index >= pageCount)
            return {};

        auto& p = pages[static_cast<size_t>(index)];
        auto* obj = new juce::DynamicObject();
        obj->setProperty("type", "NeptuneScalePage");
        obj->setProperty("version", 1);
        obj->setProperty("name", juce::String(p.getName()));
        obj->setProperty("rootNote", p.rootNote);
        obj->setProperty("mode", static_cast<int>(p.mode));

        juce::Array<juce::var> notesArr;
        for (int n = 0; n < PADS_PER_PAGE; ++n)
            notesArr.add(p.notes[n]);
        obj->setProperty("notes", notesArr);

        return juce::var(obj);
    }

    // Import a page preset, returns page index or -1 on failure
    int importPagePreset(const juce::var& v)
    {
        if (auto* obj = v.getDynamicObject())
        {
            if (obj->getProperty("type").toString() != "NeptuneScalePage")
                return -1;

            int idx = addPage(obj->getProperty("name").toString().toStdString());
            if (idx < 0)
                return -1;

            auto& p = pages[static_cast<size_t>(idx)];
            p.rootNote = (int) obj->getProperty("rootNote");
            p.mode = static_cast<Mode>((int) obj->getProperty("mode"));

            if (auto* notes = obj->getProperty("notes").getArray())
            {
                for (int n = 0; n < PADS_PER_PAGE && n < (int) notes->size(); ++n)
                    p.notes[n] = (int) (*notes)[n];
            }

            return idx;
        }
        return -1;
    }

private:
    void initDefaultPages()
    {
        // Page 0: Chromatic -- 16 consecutive semitones from root
        {
            auto& p = pages[0];
            p.name = "Chrom";
            p.rootNote = 60;  // C3
            p.mode = Mode::Relative;
            p.isFactory = true;
            for (int i = 0; i < PADS_PER_PAGE; ++i)
                p.notes[i] = i;
        }

        // Page 1: Major -- C major scale spanning ~2 octaves
        {
            auto& p = pages[1];
            p.name = "Major";
            p.rootNote = 60;
            p.mode = Mode::Relative;
            p.isFactory = true;
            static constexpr int major[] = { 0, 2, 4, 5, 7, 9, 11, 12, 14, 16, 17, 19, 21, 23, 24, 26 };
            for (int i = 0; i < PADS_PER_PAGE; ++i)
                p.notes[i] = major[i];
        }

        // Page 2: Minor (natural)
        {
            auto& p = pages[2];
            p.name = "Minor";
            p.rootNote = 60;
            p.mode = Mode::Relative;
            p.isFactory = true;
            static constexpr int minor[] = { 0, 2, 3, 5, 7, 8, 10, 12, 14, 15, 17, 19, 20, 22, 24, 26 };
            for (int i = 0; i < PADS_PER_PAGE; ++i)
                p.notes[i] = minor[i];
        }

        // Page 3: Major Pentatonic
        {
            auto& p = pages[3];
            p.name = "Penta";
            p.rootNote = 60;
            p.mode = Mode::Relative;
            p.isFactory = true;
            static constexpr int penta[] = { 0, 2, 4, 7, 9, 12, 14, 16, 19, 21, 24, 26, 28, 31, 33, 36 };
            for (int i = 0; i < PADS_PER_PAGE; ++i)
                p.notes[i] = penta[i];
        }

        // Page 4: Blues
        {
            auto& p = pages[4];
            p.name = "Blues";
            p.rootNote = 60;
            p.mode = Mode::Relative;
            p.isFactory = true;
            static constexpr int blues[] = { 0, 3, 5, 6, 7, 10, 12, 15, 17, 18, 19, 22, 24, 27, 29, 30 };
            for (int i = 0; i < PADS_PER_PAGE; ++i)
                p.notes[i] = blues[i];
        }

        // Page 5: Whole Tone
        {
            auto& p = pages[5];
            p.name = "Whole";
            p.rootNote = 60;
            p.mode = Mode::Relative;
            p.isFactory = true;
            static constexpr int whole[] = { 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30 };
            for (int i = 0; i < PADS_PER_PAGE; ++i)
                p.notes[i] = whole[i];
        }

        // Page 6: Dorian
        {
            auto& p = pages[6];
            p.name = "Dorian";
            p.rootNote = 62;  // D
            p.mode = Mode::Relative;
            p.isFactory = true;
            static constexpr int dorian[] = { 0, 2, 3, 5, 7, 9, 10, 12, 14, 15, 17, 19, 21, 22, 24, 26 };
            for (int i = 0; i < PADS_PER_PAGE; ++i)
                p.notes[i] = dorian[i];
        }

        // Page 7: Mixolydian
        {
            auto& p = pages[7];
            p.name = "Mixo";
            p.rootNote = 67;  // G
            p.mode = Mode::Relative;
            p.isFactory = true;
            static constexpr int mixo[] = { 0, 2, 4, 5, 7, 9, 10, 12, 14, 16, 17, 19, 21, 22, 24, 26 };
            for (int i = 0; i < PADS_PER_PAGE; ++i)
                p.notes[i] = mixo[i];
        }

        // Page 8: Harmonic Minor
        {
            auto& p = pages[8];
            p.name = "HrMin";
            p.rootNote = 60;
            p.mode = Mode::Relative;
            p.isFactory = true;
            static constexpr int hmin[] = { 0, 2, 3, 5, 7, 8, 11, 12, 14, 15, 17, 19, 20, 23, 24, 26 };
            for (int i = 0; i < PADS_PER_PAGE; ++i)
                p.notes[i] = hmin[i];
        }

        // Page 9: Minor Pentatonic
        {
            auto& p = pages[9];
            p.name = "mPent";
            p.rootNote = 60;
            p.mode = Mode::Relative;
            p.isFactory = true;
            static constexpr int mpent[] = { 0, 3, 5, 7, 10, 12, 15, 17, 19, 22, 24, 27, 29, 31, 34, 36 };
            for (int i = 0; i < PADS_PER_PAGE; ++i)
                p.notes[i] = mpent[i];
        }

        // Page 10: Phrygian
        {
            auto& p = pages[10];
            p.name = "Phryg";
            p.rootNote = 64;  // E
            p.mode = Mode::Relative;
            p.isFactory = true;
            static constexpr int phryg[] = { 0, 1, 3, 5, 7, 8, 10, 12, 13, 15, 17, 19, 20, 22, 24, 25 };
            for (int i = 0; i < PADS_PER_PAGE; ++i)
                p.notes[i] = phryg[i];
        }

        // Page 11: Lydian
        {
            auto& p = pages[11];
            p.name = "Lydia";
            p.rootNote = 65;  // F
            p.mode = Mode::Relative;
            p.isFactory = true;
            static constexpr int lydian[] = { 0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26 };
            for (int i = 0; i < PADS_PER_PAGE; ++i)
                p.notes[i] = lydian[i];
        }

        pageCount = DEFAULT_PAGE_COUNT;
    }

    std::array<Page, MAX_PAGES> pages {};
    int pageCount = 0;
    int activePage = 0;

    // Edit mode state
    bool editMode = false;
    int editPadIndex = -1;

    // Keyboard page selection
    KeySelectMode keySelectMode = KeySelectMode::Off;
    bool keyByPageNumber = false;
    int pageBeforeMomentary = -1;

    // Clipboard for copy/paste
    Page clipboard {};
    bool hasClipboard = false;

    // Transpose callback
    TransposeCallback transposeCallback;
};
