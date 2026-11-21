#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

// -----------------------------------------------------------------------------
// The save box: the Mac's window that asks for the name of what the controller has just saved -
// a preset (PRESET -> SV) or a project (SETTINGS -> PROJ -> SVAS). Shown here with a preset:
//
//   Save the sound of track 3 as a preset
//   [ Unknown Soldier ]                       <- the offered name, selected: typing replaces it
//   Return or Save: replaces your preset "Unknown Soldier" with this sound.
//   Escape or Cancel: nothing is saved.
//                                   [ Cancel ]  [ Save ]
//
// The words are the preset's unless the owner sets `hintFor` and `setCancelHint` (the project does).
// SaveBox is the content (what the tests build and look at); SaveBoxWindow puts it in a
// window that stays above the main window, comes to the front and takes the keyboard.
// Return = Save, Escape = Cancel, closing the window = Cancel. The line under the name says what
// Return will do with the name as typed; a name that cannot be used switches Save off.
// -----------------------------------------------------------------------------
class SaveBox : public juce::Component
{
public:
    enum class NameUse { Empty, BuiltIn, Replaces, New };

    static constexpr int WIDTH = 440, HEIGHT = 208;

    // What Return would do with the name as typed (asked on every change of the text).
    std::function<NameUse(const juce::String& typed)> nameUse;
    // The line under the name, if not the preset's words (empty answer = the preset's words).
    std::function<juce::String(NameUse, const juce::String& typed)> hintFor;
    // Save: true = done (the box can go), false = the name was not taken (the box stays).
    std::function<bool(const juce::String& typed)> onSave;
    std::function<void()> onCancel;
    // After Save succeeded or Cancel: the owner takes the window away.
    std::function<void()> onDone;

    SaveBox(const juce::String& heading, const juce::String& offeredName, int maxNameLength)
    {
        setName("Save preset");
        setOpaque(true);

        title.setText(heading, juce::dontSendNotification);
        title.setFont(juce::FontOptions(17.0f, juce::Font::bold));
        title.setColour(juce::Label::textColourId, juce::Colours::white);
        addAndMakeVisible(title);

        name.setName("name");
        name.setFont(juce::FontOptions(17.0f));
        name.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xFF101418));
        name.setColour(juce::TextEditor::textColourId, juce::Colours::white);
        name.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xFF8A9499));
        name.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xFF42A2C8));
        name.setColour(juce::TextEditor::highlightColourId, juce::Colour(0xFF2F6F8C));
        name.setColour(juce::TextEditor::highlightedTextColourId, juce::Colours::white);
        name.setInputRestrictions(maxNameLength);
        name.setSelectAllWhenFocused(true);
        name.setEscapeAndReturnKeysConsumed(false);      // they are this box's Save and Cancel
        name.setText(offeredName, juce::dontSendNotification);
        name.selectAll();
        name.onTextChange = [this] { refresh(); };
        addAndMakeVisible(name);

        hint.setFont(juce::FontOptions(14.0f));
        hint.setJustificationType(juce::Justification::topLeft);
        addAndMakeVisible(hint);

        cancelHint.setText("Escape or Cancel: nothing is saved.", juce::dontSendNotification);
        cancelHint.setFont(juce::FontOptions(14.0f));
        cancelHint.setColour(juce::Label::textColourId, juce::Colour(0xFFC8D0D4));
        addAndMakeVisible(cancelHint);

        saveButton.setButtonText("Save");
        saveButton.addShortcut(juce::KeyPress(juce::KeyPress::returnKey));
        saveButton.setColour(juce::TextButton::buttonColourId, juce::Colour(0xFF2F6F8C));
        saveButton.onClick = [this] { save(); };
        addAndMakeVisible(saveButton);

        cancelButton.setButtonText("Cancel");
        cancelButton.addShortcut(juce::KeyPress(juce::KeyPress::escapeKey));
        cancelButton.onClick = [this] { cancel(); };
        addAndMakeVisible(cancelButton);

        setSize(WIDTH, HEIGHT);
        refresh();
    }

    juce::TextEditor& getNameEditor()     { return name; }
    juce::TextButton& getSaveButton()     { return saveButton; }
    juce::TextButton& getCancelButton()   { return cancelButton; }
    juce::String getHeading() const       { return title.getText(); }
    juce::String getHint() const          { return hint.getText(); }
    juce::String getCancelHint() const    { return cancelHint.getText(); }
    void setCancelHint(const juce::String& text) { cancelHint.setText(text, juce::dontSendNotification); }
    bool isAnswered() const               { return answered; }

    // The line under the name and the Save button follow the text (also after `nameUse` is set).
    void refresh()
    {
        const auto typed = name.getText().trim();
        const auto use = typed.isEmpty() ? NameUse::Empty : (nameUse ? nameUse(typed) : NameUse::New);
        juce::String text = hintFor ? hintFor(use, typed) : juce::String();
        if (text.isEmpty()) switch (use)
        {
            case NameUse::Empty:    text = "Type a name."; break;
            case NameUse::BuiltIn:  text = "\"" + typed + "\" is a built-in preset: choose another name."; break;
            case NameUse::Replaces: text = "Return or Save: replaces your preset \"" + typed + "\" with this sound."; break;
            case NameUse::New:      text = "Return or Save: keeps it as a new preset \"" + typed + "\"."; break;
        }
        hint.setText(text, juce::dontSendNotification);
        const bool usable = use == NameUse::Replaces || use == NameUse::New;
        hint.setColour(juce::Label::textColourId, usable ? juce::Colour(0xFFC8D0D4) : juce::Colour(0xFFFFB04A));
        saveButton.setEnabled(usable);
    }

    void save()
    {
        refresh();
        if (answered || !saveButton.isEnabled())
            return;
        if (onSave && !onSave(name.getText()))
        {
            refresh();
            return;
        }
        answered = true;
        if (onDone) onDone();
    }

    void cancel()
    {
        if (answered)
            return;
        answered = true;
        if (onCancel) onCancel();
        if (onDone) onDone();
    }

    // The keyboard goes to the name, with the whole name selected.
    void focusName()
    {
        name.grabKeyboardFocus();
        name.selectAll();
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress(juce::KeyPress::returnKey)) { save();   return true; }
        if (key == juce::KeyPress(juce::KeyPress::escapeKey)) { cancel(); return true; }
        return false;
    }

    void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xFF263238)); }

    void resized() override
    {
        auto area = getLocalBounds().reduced(20, 16);
        title.setBounds(area.removeFromTop(26));
        area.removeFromTop(8);
        name.setBounds(area.removeFromTop(32));
        area.removeFromTop(8);
        hint.setBounds(area.removeFromTop(22));
        cancelHint.setBounds(area.removeFromTop(22));
        auto buttons = area.removeFromBottom(36);
        saveButton.setBounds(buttons.removeFromRight(110));
        buttons.removeFromRight(12);
        cancelButton.setBounds(buttons.removeFromRight(110));
    }

    // The box as it is drawn, as a PNG (for looking at it: the tests and NEPTUNE_SNAPSHOT_SAVE_BOX).
    bool writePng(const juce::File& file, float scale = 2.0f)
    {
        const auto image = createComponentSnapshot(getLocalBounds(), true, scale);
        file.deleteFile();
        juce::FileOutputStream out(file);
        return out.openedOk() && juce::PNGImageFormat().writeImageToStream(image, out);
    }

private:
    juce::Label title, hint, cancelHint;
    juce::TextEditor name;
    juce::TextButton saveButton, cancelButton;
    bool answered = false;
};

// The box in its window: above the main window (and everything else, so it cannot open behind
// something), with the system's title bar; its close button is Cancel.
class SaveBoxWindow : public juce::DocumentWindow
{
public:
    SaveBoxWindow(const juce::String& heading, const juce::String& offeredName, int maxNameLength,
                  const juce::String& windowTitle = "Save preset")
        : DocumentWindow(windowTitle, juce::Colour(0xFF263238), DocumentWindow::closeButton),
          box(heading, offeredName, maxNameLength)
    {
        setUsingNativeTitleBar(true);
        setContentNonOwned(&box, true);
        setResizable(false, false);
        setAlwaysOnTop(true);
    }

    SaveBox& getBox() { return box; }

    // Shown over `over` (the main window), in front, with the keyboard in the name. The app is
    // brought to the front as well: SV comes from the controller while any app may be in front.
    void showOver(juce::Component* over)
    {
        centreAroundComponent(over, getWidth(), getHeight());
        setVisible(true);
        takeKeyboard();
        // Once more when the window is really up (a window that has just been made may not be
        // the key window yet when the focus is first asked for).
        juce::Component::SafePointer<SaveBoxWindow> safe(this);
        juce::Timer::callAfterDelay(150, [safe]
        {
            if (safe != nullptr && !safe->box.isAnswered() && !safe->box.getNameEditor().hasKeyboardFocus(true))
                safe->takeKeyboard();
        });
    }

    void closeButtonPressed() override { box.cancel(); }

private:
    void takeKeyboard()
    {
        juce::Process::makeForegroundProcess();
        toFront(true);
        box.focusName();
    }

    SaveBox box;
};
