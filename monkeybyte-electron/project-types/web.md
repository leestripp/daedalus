---
id: web
name: Web (HTML/JS)
description: HTML/CSS/JavaScript web project
detect: package.json, index.html, vite.config.js
---

This is a web project. Conventions:

- Check package.json scripts and the framework in use (React, Vue, vanilla,
  etc.) before writing code; match the framework's idioms.
- Match the existing module system (ESM vs CJS), lint config (eslint /
  prettier), and code style.
- Prefer small, dependency-free solutions; ask before adding npm packages.
- Verify with the project's own dev server / build / test commands
  (npm run dev, npm test, npm run build) rather than inventing new ones.
- For UI work, you can open the dev server in MonkeyByte's browser tab
  (app_ui_control browser.open) and inspect the live page to verify.
