import { startReporter } from './reporter';

// Ink belongs to the document: origin + path + query (a #hash is the same document).
startReporter(location.origin + location.pathname + location.search);
