import { boot } from './ink/overlay';

// Ink belongs to the document: origin + path + query (a #hash is the same document).
boot({ docKey: () => location.origin + location.pathname + location.search });
