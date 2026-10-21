# Chrome Web Store listing

Copy each part into the matching box in the Chrome Web Store Developer Dashboard.

## Store listing tab

**Name** (comes from the extension itself): ScrollPen

**Summary** (from the manifest, max 132 characters):
Draw on any web page or PDF. Your ink stays on the page as you scroll, zoom and come back later. Nothing leaves your computer.

**Category:** Productivity → Tools

**Language:** English

**Description:**

```
ScrollPen is a pen for the web. Draw, highlight and write on any page or PDF in Chrome, and your ink stays exactly where you put it: scroll, zoom or come back tomorrow, and it's still on the same words.

Great for studying, teaching, presenting, giving feedback on a design, or marking up an article.

TOOLS
• Pen (with pressure for pen tablets), highlighter, line, arrow, rectangle, ellipse and text
• Laser pointer that fades away, for presenting
• Eraser (or use your pen's eraser end), undo and redo
• Whiteboard and blackboard for quick sketches
• 9 colours, adjustable sizes, filled shapes
• Screenshot of the page with your ink, saved to your Downloads

INK THAT STAYS PUT
• Ink is attached to the page, not the screen: it scrolls and zooms with the content
• Works on scrolling panels inside pages (chats, documents, code viewers) too
• PDFs open in ScrollPen's own viewer so you can draw on them
• Your ink is saved for each page and comes back when you open it again
• Prefer ink fixed to the screen? Switch "follow scrolling" off

QUICK TO USE
• Click the ScrollPen button or press Alt+Shift+D to start drawing; Esc to use the page normally again
• Keys while drawing: P pen, H highlighter, L laser, E eraser, I line, A arrow, R rectangle, O ellipse, T text, W whiteboard, K blackboard, 1-9 colours, [ ] size, Ctrl+Z undo
• Hold Caps Lock to click links without leaving the pen
• The mouse wheel keeps scrolling the page while you draw
• Move or shrink the toolbar to keep it out of the way

PRIVATE BY DESIGN
• Everything stays on your computer, in your Chrome profile
• No account, no servers, no analytics, no ads
• Ink is saved locally so it's there when you return; delete any of it from ScrollPen's settings at any time
```

**Icon:** `extensions/scrollpen/icons/icon128.png`

**Screenshots** (1280×800, made by `npm run store:screenshots`, found in `release/store/`):
1. `1-annotate-a-page.png`
2. `2-ink-stays-when-you-scroll.png`
3. `3-draw-on-pdfs.png`
4. `4-whiteboard.png`

**Small promo tile** (440×280): optional; skip it.

## Privacy tab

**Single purpose:**

```
ScrollPen lets people draw and write on web pages and PDFs in Chrome, with the ink attached to the page so it stays in place while scrolling and is there again when the page is reopened.
```

**Permission justifications:**

| Permission | Justification to paste |
|---|---|
| storage | Saves the user's ink for each page and their settings (colours, sizes) on their own computer, so ink is still there when they reopen the page. |
| unlimitedStorage | People who draw on many pages or long PDFs can exceed the default storage limit; without it their saved ink would stop being saved. All storage stays on the user's computer. |
| activeTab | Takes a screenshot of the current tab (with the user's ink) when the user clicks ScrollPen's screenshot button or shortcut. |
| scripting | When the user clicks the ScrollPen button on a tab that was already open before ScrollPen was installed, ScrollPen starts its drawing layer in that tab. |
| Host permissions (http, https, file) | The drawing layer has to run on whatever page the user wants to draw on, and it shows the user's saved ink when they return to a page. To let users draw on PDFs, ScrollPen opens PDF links in its own viewer, which needs to read the PDF from the site (or a local file) it came from. Nothing is sent anywhere. |

**Are you using remote code?** No, I am not using remote code.

**Data usage:** ScrollPen keeps everything on the user's computer and sends nothing anywhere, so it does not collect user data. Leave every data type unticked, then tick the three certifications:
- I do not sell or transfer user data to third parties, outside of the approved use cases
- I do not use or transfer user data for purposes that are unrelated to my item's single purpose
- I do not use or transfer user data to determine creditworthiness or for lending purposes

**Privacy policy URL:** the public link to `store/privacy-policy.md` (see PUBLISHING.md, step 4).

## Distribution tab

- Payments: Free
- Visibility: Public (or Unlisted to try it with friends first)
- Regions: All regions
