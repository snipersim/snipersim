"""Stop at the first failing check, for debugging.

A regression pass deliberately runs everything: the value of a pass is the whole
picture, and stopping early hides the rest of it.  That is the wrong trade once
the picture is already known and one check is being worked on -- then waiting out
the other three hundred is just waiting, and the state that would have helped,
the work directory the check ran in, has been deleted by the time it finishes.

--failfast stops at the first failing check rather than the first failing suite.
Suite granularity would not help: a config that breaks two hundred checks into
`configs` would still cost the remaining hundred-odd before anything stopped.

The suites keep their work directory when they stop this way, and say where it
is.  A check that failed and left nothing behind is the case this option exists
to avoid.
"""


class FailFast(Exception):
    """Raised at a failing check to unwind out of nested loops."""


def add_argument(ap):
    ap.add_argument('--failfast', action='store_true',
                    help='stop at the first failing check and keep its work '
                         'directory (for debugging; a regression pass wants '
                         'the whole picture and should not use this)')
