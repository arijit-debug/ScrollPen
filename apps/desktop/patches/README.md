# Patched dependencies

`npm` uses these instead of the registry packages (see `overrides` in `../package.json`).
Drop each one as soon as upstream publishes a fixed release.

- **braces 3.0.3** (MIT, via micromatch in metro and jest): GHSA-vfj7-8cjw-p6xm, a stack
  overflow on deeply nested patterns, has no fixed release. The patch refuses patterns nested
  more than 256 levels deep with a `SyntaxError`, the same way braces already refuses
  over-long input. Changes: `lib/parse.js` (`enter()` before each brace/paren block) and
  `lib/constants.js` (`MAX_DEPTH`).
