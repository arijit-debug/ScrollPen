# Publishing ScrollPen on the Chrome Web Store

For a first-timer, step by step. ScrollPen is free, so there's nothing to set up for payments.
Plan on about an hour of work, then Google's review: usually a few business days, but
ScrollPen runs on every website, which triggers a closer manual review that can take longer,
sometimes a couple of weeks.

Already done for you in the extension: Manifest V3, no remote code, only the permissions it
needs (each one justified in `listing.md`), a privacy policy, the in-product "Save your ink on
this computer?" consent Google requires, a settings page that shows and exports everything
stored, store screenshots, and the licences of the open-source code it includes.

## 1. Make your GitHub repository public

The privacy policy link and the "Source code" link inside ScrollPen point to GitHub, so the
repository must be public before you submit.

1. Decide on a licence (see the note at the end of this file) and add it first if you want one.
2. Go to https://github.com/arijit-debug/ScrollPen → **Settings** → scroll to the bottom ("Danger Zone") → **Change visibility** → **Make public**, and confirm.
3. Check that this link opens when you're signed out (for example in an Incognito window):
   https://github.com/arijit-debug/ScrollPen/blob/main/store/privacy-policy.md

Good to know: once public, anyone can see every past commit, including the email address each
commit was made with. This repository now commits with GitHub's private "noreply" address.

## 2. Make the upload file and the screenshots

In PowerShell, in the `E:\ScrollPen` folder:

```powershell
npm run ext:pack     # -> release\scrollpen-1.0.0.zip   (the file you upload)
```

The 4 screenshots are in `release\store\` (Claude can regenerate them any time).

## 3. Create your developer account (one time, US$5)

1. **Turn on 2-Step Verification** for the Google account you'll publish with
   (https://myaccount.google.com/security). Google requires it for publishing.
   Tip: a separate Google account just for publishing keeps your personal Gmail private.
2. Go to https://chrome.google.com/webstore/devconsole and sign in.
3. Accept the developer agreement and pay the one-time **US$5** fee.
4. On the **Account** page:
   - **Publisher name:** what users see, e.g. your name or "ScrollPen".
   - **Contact email:** Google sends a link to verify it. Users may see it, so you can use a separate address.
   - **Trader / non-trader declaration:** choose **non-trader**. ScrollPen is free and not part of a business. (Traders must publish their address and phone number for EU users.)

## 4. Upload and fill in the listing

1. In the Developer Dashboard click **New item** and upload `release\scrollpen-1.0.0.zip`.
   You'll likely see a notice that ScrollPen's site access means an in-depth review. That's
   expected and fine.
2. Fill in each tab using `store\listing.md` (copy and paste):
   - **Store listing:** description, category (Productivity → Tools), language, the 128×128
     icon (`extensions\scrollpen\icons\icon128.png`), the 4 screenshots, homepage and support URLs.
   - **Privacy:** single purpose; one justification per permission; "No, I am not using remote
     code"; the data usage boxes **exactly as the table in listing.md says** (tick *Web history*
     and *Website content*, plus the three certifications); the privacy policy URL.
   - **Test instructions:** paste the text from listing.md (no account is needed).
   - **Distribution:** Free, **Public**, all regions. (Pick **Unlisted** instead if you want only
     people with the link to find it at first.)
3. Click **Submit for review**. You can tick "publish automatically after approval".

## 5. While you wait, and after

- Google emails you when it's approved, or explains what to change if it's rejected. Common
  reasons are a permission justification they don't accept or a listing that doesn't match
  what the extension does; send Claude the email and it will fix it.
- Once published, people install ScrollPen with one click from its store page, and Chrome
  updates it for them automatically.
- To get users: share the store link in your WhatsApp groups, with classmates and teachers, on
  Reddit communities about studying or note-taking, and ask happy users for a review. Feedback
  can come in through GitHub Issues.

## Updating later

1. Raise the version in **both** `extensions\scrollpen\manifest.json` and
   `extensions\scrollpen\package.json` (for example `1.0.0` → `1.0.1`). The store refuses an
   upload with the same version.
2. `npm run ext:pack`
3. Dashboard → ScrollPen → **Package** → **Upload new package** → **Submit for review**.
4. If an update changes what ScrollPen stores, update `privacy-policy.md`, the Privacy tab
   answers and the in-product disclosure (`src/ink/disclosure.ts`) first: Google requires
   users to be told inside the extension before new data practices start.

## Note: a licence for the public repository

Without a licence, a public repository is "look but don't touch": people can read the code but
have no right to reuse it. Most free extensions use the **MIT licence** (anyone may use, copy
and change the code, as long as they keep your copyright notice). It's your choice; ask Claude
to add one.

## Sources

- Program policies and user data FAQ: https://developer.chrome.com/docs/webstore/program-policies/user-data-faq
- Disclosure and consent requirements: https://developer.chrome.com/docs/webstore/program-policies/disclosure-requirements
- Privacy tab fields: https://developer.chrome.com/docs/webstore/cws-dashboard-privacy
