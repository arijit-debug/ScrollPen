# Publishing ScrollPen on the Chrome Web Store

Step by step, for a first time. Plan on about an hour, plus Google's review (usually a few
days; extensions that can run on every website, like ScrollPen, can take a little longer).

## 1. Make the upload file and the screenshots

In PowerShell, in the `E:\ScrollPen` folder:

```powershell
npm run ext:pack            # -> release\scrollpen-1.0.0.zip   (the file you upload)
npm run store:screenshots   # -> release\store\*.png           (4 screenshots)
```

## 2. Try it in your own Chrome first

1. Open `chrome://extensions` in Chrome.
2. Turn on **Developer mode** (top right).
3. Click **Load unpacked** and choose the folder `E:\ScrollPen\extensions\scrollpen\dist`.
4. Open any website, click the puzzle-piece icon in Chrome's toolbar, pin **ScrollPen**, and click it (or press Alt+Shift+D). Draw, scroll, reload the page.
5. For PDFs and pictures on your computer: on `chrome://extensions`, click **Details** under ScrollPen and turn on **Allow access to file URLs**.

When you're happy, remove this test copy (click **Remove**) before installing the published one, so you don't have two.

## 3. Create your developer account (one time, US$5)

1. Go to https://chrome.google.com/webstore/devconsole and sign in with the Google account you want to publish with.
2. Accept the developer agreement and pay the one-time US$5 registration fee.
3. Fill in the account page: a publisher name (shown in the store) and a contact email, which Google verifies.

## 4. Put the privacy policy online

The store needs a public web link to a privacy policy. The simplest free way:

1. Open `store\privacy-policy.md`, replace `[YOUR CONTACT EMAIL]` with the email you want people to use, and copy all of the text.
2. Go to https://gist.github.com (signed in to GitHub), paste it, name the file `privacy-policy.md`, and click **Create public gist**.
3. Copy the gist's address; that's your privacy policy URL.

(Your ScrollPen repository is private, so a link into it wouldn't work for others.)

## 5. Upload

1. In the Developer Dashboard click **New item** (or **Add new item**) and upload `release\scrollpen-1.0.0.zip`.
2. Fill in the tabs using `store\listing.md`: copy each part into the matching box.
   - **Store listing**: description, category, language, icon, screenshots.
   - **Privacy**: single purpose, a justification for each permission, "no remote code", data usage and the privacy policy URL.
   - **Distribution**: Free, Public (or Unlisted to share it only with a link first), all regions.
3. Click **Submit for review**.

Google emails you when it's approved or if they need changes. Once approved, people can install ScrollPen from its store page with one click, and Chrome keeps it updated automatically.

## Updating later

1. Raise the version number in **both** `extensions\scrollpen\manifest.json` and `extensions\scrollpen\package.json` (for example `1.0.0` → `1.0.1`). The store refuses an upload with the same version.
2. `npm run ext:pack`
3. In the Developer Dashboard open ScrollPen → **Package** → **Upload new package**, then **Submit for review**.
