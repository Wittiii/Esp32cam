"""Bounded local application logs; never reads or logs configuration values."""

import logging
from logging.handlers import RotatingFileHandler
import os
from pathlib import Path
import sys
import threading
import traceback


class LocalLogHandler(RotatingFileHandler):
    """Disable file logging on an I/O failure, leaving console logging available."""

    failed = False

    def _open(self):
        def opener(path, flags):
            return os.open(path, flags, 0o600)
        return open(self.baseFilename, self.mode, encoding=self.encoding, opener=opener)

    def emit(self, record):
        if not self.failed:
            super().emit(record)

    def handleError(self, record):
        self.failed = True
        sys.stderr.write("Local file logging disabled after an I/O error; restart service to retry.\n")


def configure_logging(directory: Path, level: str) -> None:
    log_format = "%(asctime)s %(levelname)s %(name)s: %(message)s"
    formatter = logging.Formatter(log_format)
    logging.basicConfig(level=getattr(logging, level.upper(), logging.INFO),
                        format=log_format)
    log = logging.getLogger("pi-cam-controller")
    try:
        directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        handler = LocalLogHandler(directory / "controller.log", maxBytes=2 * 1024 * 1024,
                                  backupCount=4, encoding="utf-8")
        handler.setFormatter(formatter)
        logging.getLogger().addHandler(handler)
    except OSError as exc:
        log.warning("Local file logging unavailable: %s", exc)

    def uncaught(kind, value, tb):
        log.critical("Unhandled application exception", exc_info=(kind, value, tb))

    def thread_uncaught(args):
        log.critical("Unhandled exception in thread %s", args.thread.name,
                     exc_info=(args.exc_type, args.exc_value, args.exc_traceback))

    sys.excepthook = uncaught
    threading.excepthook = thread_uncaught
    log.info("Controller starting: pid=%s python=%s", os.getpid(), sys.version.split()[0])


def log_thread_stacks() -> None:
    log = logging.getLogger("pi-cam-controller")
    names = {thread.ident: thread.name for thread in threading.enumerate()}
    for ident, frame in sys._current_frames().items():
        log.error("Thread stack %s (%s):\n%s", names.get(ident, "unknown"), ident,
                  "".join(traceback.format_stack(frame, limit=32)))
