#!/usr/bin/env python3
"""
ida_monitor.py - IDA TDI Camera Monitor

Reads 8-bit grayscale preview images from /dev/shm/ida_preview_N
(written by ida_app after each swath) and displays all 4 channels.

Read-only: no ioctl, no write to /dev/shm, no interaction with DMA.

Requirements:
    pip install PyQt5
"""

import sys
import os
import mmap
import struct

from PyQt5.QtWidgets import (
    QApplication, QMainWindow, QWidget,
    QHBoxLayout, QVBoxLayout, QLabel,
    QScrollArea, QFrame, QSizePolicy,
    QStatusBar,
)
from PyQt5.QtCore import QTimer, Qt
from PyQt5.QtGui import QImage, QPixmap, QFont

# ------------------------------------------------------------------ #
# Constants – must match include/ida_preview.h                        #
# ------------------------------------------------------------------ #
PREVIEW_MAGIC     = 0x49444150          # "IDAP"
PREVIEW_MAX_W     = 2048
PREVIEW_MAX_H     = 8192
PREVIEW_DATA_SIZE = PREVIEW_MAX_W * PREVIEW_MAX_H

# struct ida_preview_header layout (little-endian, 64 bytes):
#   uint32 magic, seq, width, height
#   uint64 orig_lines, orig_bytes
#   int32  crc_error, state, error
#   uint32 _pad[5]
_HDR_FMT  = '<IIIIQQiii20s'
_HDR_SIZE = struct.calcsize(_HDR_FMT)   # must be 64
assert _HDR_SIZE == 64, f"Header size mismatch: {_HDR_SIZE}"

PREVIEW_SHM_SIZE = _HDR_SIZE + PREVIEW_DATA_SIZE
SHM_DIR          = '/dev/shm'
POLL_INTERVAL_MS = 200   # 5 Hz refresh

# ------------------------------------------------------------------ #
# Per-channel panel                                                   #
# ------------------------------------------------------------------ #

class ChannelPanel(QFrame):
    """Displays one TDI channel: status bar + scrollable strip image."""

    def __init__(self, ch_id: int, parent=None):
        super().__init__(parent)
        self.ch_id    = ch_id
        self.last_seq = None   # None = never received a frame
        self._mmap    = None
        self._mfile   = None

        self._build_ui()
        self._open_shm()

    # -------------------------------------------------------------- #
    # UI construction                                                  #
    # -------------------------------------------------------------- #

    # Badge stylesheet helpers
    @staticmethod
    def _badge(text: str, bg: str, fg: str = '#fff') -> str:
        """Return HTML for a small rounded badge."""
        return (
            f'<span style="background:{bg};color:{fg};'
            f'border-radius:3px;padding:1px 6px;font-size:11px;">'
            f'{text}</span>'
        )

    def _build_ui(self):
        self.setFrameShape(QFrame.StyledPanel)
        self.setMinimumWidth(220)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)

        outer = QVBoxLayout(self)
        outer.setContentsMargins(6, 6, 6, 6)
        outer.setSpacing(4)

        # ---- Title ----
        title = QLabel(f'Channel {self.ch_id}')
        title.setAlignment(Qt.AlignCenter)
        f = QFont()
        f.setBold(True)
        f.setPointSize(11)
        title.setFont(f)
        outer.addWidget(title)

        # ---- Info bar (light background card) ----
        info_card = QFrame()
        info_card.setStyleSheet(
            'QFrame { background: #f5f5f5; border: 1px solid #ddd;'
            ' border-radius: 5px; }'
        )
        card_layout = QVBoxLayout(info_card)
        card_layout.setContentsMargins(8, 5, 8, 5)
        card_layout.setSpacing(3)

        # Row 1: seq  |  crc badge  |  state badge
        row1 = QHBoxLayout()
        row1.setSpacing(6)

        self.lbl_seq = QLabel('SEQ —')
        self.lbl_seq.setStyleSheet(
            'font-family: monospace; font-size: 11px; color: #444;'
        )
        row1.addWidget(self.lbl_seq)
        row1.addStretch()

        self.lbl_crc = QLabel(self._badge('CRC —', '#aaa'))
        self.lbl_crc.setTextFormat(Qt.RichText)
        row1.addWidget(self.lbl_crc)

        self.lbl_state = QLabel(self._badge('—', '#aaa'))
        self.lbl_state.setTextFormat(Qt.RichText)
        row1.addWidget(self.lbl_state)

        card_layout.addLayout(row1)

        # Row 2: pixel dimensions + data size
        self.lbl_geo = QLabel('— × — lines   — MB')
        self.lbl_geo.setAlignment(Qt.AlignCenter)
        self.lbl_geo.setStyleSheet('color: #666; font-size: 10px;')
        card_layout.addWidget(self.lbl_geo)

        outer.addWidget(info_card)

        # ---- Scrollable image area ----
        self.scroll = QScrollArea()
        self.scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.scroll.setVerticalScrollBarPolicy(Qt.ScrollBarAsNeeded)
        self.scroll.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)

        self.lbl_image = QLabel()
        self.lbl_image.setAlignment(Qt.AlignHCenter | Qt.AlignTop)
        self.lbl_image.setSizePolicy(QSizePolicy.Fixed, QSizePolicy.Fixed)
        self.lbl_image.setStyleSheet('background-color: #e8e8e8;')

        self.scroll.setWidget(self.lbl_image)
        self.scroll.setWidgetResizable(False)
        outer.addWidget(self.scroll, stretch=1)

    # -------------------------------------------------------------- #
    # Shared memory                                                    #
    # -------------------------------------------------------------- #

    def _open_shm(self):
        path = os.path.join(SHM_DIR, f'ida_preview_{self.ch_id}')
        try:
            self._mfile = open(path, 'rb')
            self._mmap  = mmap.mmap(
                self._mfile.fileno(),
                PREVIEW_SHM_SIZE,
                access=mmap.ACCESS_READ,
            )
        except OSError:
            self._mfile = None
            self._mmap  = None

    def _close_shm(self):
        if self._mmap:
            self._mmap.close()
            self._mmap = None
        if self._mfile:
            self._mfile.close()
            self._mfile = None 

    # -------------------------------------------------------------- #
    # Polling                                                          #
    # -------------------------------------------------------------- #

    def poll(self):
        """Called by the main timer. Re-opens shm if not yet available."""
        if self._mmap is None:
            self._open_shm()
            if self._mmap is None:
                self.lbl_geo.setText(
                    f'ida_preview_{self.ch_id} not found'
                )
                return

        # Read header
        try:
            self._mmap.seek(0)
            raw = self._mmap.read(_HDR_SIZE)
        except Exception:
            self._close_shm()
            return

        (magic, seq, width, height,
         orig_lines, orig_bytes,
         crc, state, error, _pad) = struct.unpack(_HDR_FMT, raw)

        if magic != PREVIEW_MAGIC:
            self.lbl_geo.setText('Bad magic – shm corrupt?')
            return

        # seq == 0 means ida_app initialised the buffer but no swath yet
        if seq == 0:
            self.lbl_geo.setText('Waiting for first swath…')
            return

        if seq == self.last_seq:
            return  # no new frame

        self.last_seq = seq

        # Read pixel data
        try:
            pixel_bytes = bytes(self._mmap.read(width * height))
        except Exception:
            return

        if len(pixel_bytes) < width * height:
            return

        self._update_image(pixel_bytes, width, height)
        self._update_status(seq, width, height, orig_lines,
                            orig_bytes, crc, state, error)

    # -------------------------------------------------------------- #
    # Display helpers                                                  #
    # -------------------------------------------------------------- #

    def _update_image(self, pixel_bytes: bytes, width: int, height: int):
        img = QImage(pixel_bytes, width, height, width,
                     QImage.Format_Grayscale8)

        # Scale to fit the scroll-area viewport width
        vp_w = self.scroll.viewport().width() - 2
        if vp_w < 16:
            vp_w = 200

        pixmap   = QPixmap.fromImage(img)
        scaled   = pixmap.scaledToWidth(vp_w, Qt.SmoothTransformation)
        self.lbl_image.setPixmap(scaled)
        self.lbl_image.resize(scaled.size())

    def _update_status(self, seq, width, height,
                       orig_lines, orig_bytes, crc, state, error):
        mb = orig_bytes / (1024 * 1024)

        # SEQ
        self.lbl_seq.setText(f'SEQ {seq}')

        # CRC badge
        if crc == 0:
            self.lbl_crc.setText(self._badge('✓ CRC OK', '#27ae60'))
        else:
            self.lbl_crc.setText(self._badge(f'✗ CRC {crc}', '#e74c3c'))

        # State badge
        if error != 0:
            self.lbl_state.setText(self._badge(f'ERR {error}', '#c0392b'))
        elif state == -2:
            self.lbl_state.setText(self._badge('Normal', '#2980b9'))
        elif state == -3:
            self.lbl_state.setText(self._badge('Early-end', '#e67e22'))
        else:
            self.lbl_state.setText(self._badge(f'State {state}', '#888'))

        # Geometry
        subsample = ''
        if height > 0 and orig_lines > height:
            subsample = f'  1:{orig_lines // height}↓'
        self.lbl_geo.setText(
            f'{width} × {orig_lines} lines{subsample}   {mb:.1f} MB'
        )

    def closeEvent(self, event):
        self._close_shm()
        super().closeEvent(event)

    def resizeEvent(self, event):
        """Re-scale the current image when the panel is resized."""
        super().resizeEvent(event)
        if self.lbl_image.pixmap() and not self.lbl_image.pixmap().isNull():
            vp_w = self.scroll.viewport().width() - 2
            if vp_w > 16:
                scaled = self.lbl_image.pixmap().scaledToWidth(
                    vp_w, Qt.SmoothTransformation)
                self.lbl_image.setPixmap(scaled)
                self.lbl_image.resize(scaled.size())


# ------------------------------------------------------------------ #
# Main window                                                         #
# ------------------------------------------------------------------ #

class IDAMonitor(QMainWindow):
    def __init__(self, n_channels: int = 4):
        super().__init__()
        self.setWindowTitle('IDA TDI Monitor')
        self.resize(1280, 820)

        central = QWidget()
        self.setCentralWidget(central)
        row = QHBoxLayout(central)
        row.setSpacing(8)
        row.setContentsMargins(8, 8, 8, 8)

        self.panels: list[ChannelPanel] = []
        for i in range(n_channels):
            panel = ChannelPanel(i)
            row.addWidget(panel)
            self.panels.append(panel)

        self.statusBar().showMessage(
            f'Monitoring {n_channels} channels  •  '
            f'Poll interval {POLL_INTERVAL_MS} ms  •  read-only'
        )

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._poll_all)
        self._timer.start(POLL_INTERVAL_MS)

    def _poll_all(self):
        for panel in self.panels:
            panel.poll()


# ------------------------------------------------------------------ #
# Entry point                                                         #
# ------------------------------------------------------------------ #

def main():
    app = QApplication(sys.argv)
    app.setStyle('Fusion')

    app.setPalette(app.style().standardPalette())

    win = IDAMonitor(n_channels=4)
    win.show()
    sys.exit(app.exec_())


if __name__ == '__main__':
    main()
