//! About
// About: what the runtime tells an app about itself. A view with no keys of its own.
const v = os.sandbox.version, a = os.sandbox.app;
return { sel: -1, hint: 'Esc back', rows: () => [
  'TinyOS ' + a.version + ' (app id ' + a.id + ')',
  'Runtime ' + v.runtime + ', API ' + v.api,
  'Grants: ' + os.sandbox.capabilities.join(' '),
  'Uptime: ' + Math.floor(os.sandbox.now() / 1e6) + ' s',
  'Edit the .JS files on the card and reboot.',
] };
