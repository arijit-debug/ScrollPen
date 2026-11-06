// The in-product disclosure shown before ScrollPen stores anything (Chrome Web Store user
// data policy: disclose and get consent in the product, even for data kept on the device).
// The toolbar's first-use panel and the settings page both use this text; keep it in step
// with store/privacy-policy.md and the Privacy tab answers in store/listing.md.

export const DISCLOSURE = {
  title: 'Save your ink on this computer?',
  intro:
    "ScrollPen can keep your ink so it's still there when you come back to a page. To do that, it saves these on this computer, in this Chrome profile only:",
  items: [
    'your ink: drawings, shapes and the text you write',
    'the address and title of each page you draw on',
    'when you last drew on that page',
  ],
  never:
    'Nothing is ever sent anywhere: ScrollPen has no servers, no account and no tracking. You can see, download or delete everything in ScrollPen\'s settings, and change this choice there.',
  save: 'Save my ink on this computer',
  tab: "Don't save (ink is gone when I close the tab)",
} as const;
