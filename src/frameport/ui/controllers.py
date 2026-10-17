"""Bundled controller mapping illustrations, installed with the other UI SVG assets at startup."""
from __future__ import annotations

import flet as ft

from ..artwork.thumbs import asset_url
from ..core.paths import user_data_dir
from ..i18n import tr
from . import glyphs as G
from . import theme as T


def diagram_source(mode: int) -> str:
    return asset_url(user_data_dir() / G.FOLDER / f"quest-buttons-{mode}.svg")


def diagram(mode: int) -> ft.Image:
    return ft.Image(src=diagram_source(mode), width=T.px(440), height=T.px(283), fit=ft.BoxFit.CONTAIN,
                    semantics_label=tr("Frame controller buttons: physical input above, Quest Touch input below. "
                                       "A dash means unbound."))
