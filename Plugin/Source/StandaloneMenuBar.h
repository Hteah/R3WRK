#pragma once
#include <JuceHeader.h>
#include "EditorToolbar.h"

/**
    The Standalone app's native macOS application menu bar: File / Edit / Tools, holding the
    same actions as the in-window "Tools ▾" button. Both surfaces build their menus from
    EditorToolbar::ToolsMenuItem ids and dispatch through EditorToolbar::performToolsItem(),
    so there's one source of truth for what each action does.

    Installed by PluginEditor via juce::MenuBarModel::setMacMainMenu() only when
    wrapperType == Standalone -- in a plugin the host owns the menu bar. macOS asks for each
    top menu's contents every time it's opened, so enable / tick state is always current
    without this model having to observe the document.
*/
class StandaloneMenuBar : public juce::MenuBarModel
{
public:
    explicit StandaloneMenuBar (EditorToolbar& tb) : toolbar (tb) {}

    juce::StringArray getMenuBarNames() override
    {
        return { "File", "Edit", "Tools" };
    }

    juce::PopupMenu getMenuForIndex (int topLevelMenuIndex, const juce::String& /*name*/) override
    {
        juce::PopupMenu menu;
        switch (topLevelMenuIndex)
        {
            case 0: toolbar.buildMenuBarMenu (menu, EditorToolbar::ToolsMenuGroup::file);  break;
            case 1: toolbar.buildMenuBarMenu (menu, EditorToolbar::ToolsMenuGroup::edit);  break;
            case 2: toolbar.buildMenuBarMenu (menu, EditorToolbar::ToolsMenuGroup::tools); break;
            default: break;
        }
        return menu;
    }

    void menuItemSelected (int menuItemID, int /*topLevelMenuIndex*/) override
    {
        if (menuItemID > 0)
            toolbar.performToolsItem (menuItemID);
    }

private:
    EditorToolbar& toolbar;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StandaloneMenuBar)
};
