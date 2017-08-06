# -*- coding: utf-8 -*-
# Licensed under The MIT License

from PIL import Image, ImageFont, ImageDraw

from ..util import create_image
from ..util import PROJECT_DIR
from ..effect import Effect, register


class TextStay(Effect):

  def __init__(self, text='FISH', duration=None, speed=1, color='red', alpha=255):
    if duration is None:
        duration = speed * 20
    Effect.__init__(self, duration=duration, mirror=False)
    self._text = text
    self._color = color
    self._alpha = alpha

  def _prepare(self):
    # More founts around: '/usr/share/fonts/truetype/'
    font = ImageFont.truetype(
        PROJECT_DIR + '/dfplayer/effects/DejaVuSans-Bold.ttf',
        int(self._height * 1))
    (w, h) = font.getsize(self._text)
    self._rendered_text = create_image(
        int(w * 1.2), self._height, 'black', self._alpha)
    draw = ImageDraw.Draw(self._rendered_text)
    draw.text((int(w * 0.2), -1), self._text, self._color, font=font)

  def get_image(self, elapsed, **kwargs):
    image = self._create_image('black', self._alpha)
    rendered = self._rendered_text
    rendered = rendered.resize((100, self._height))
    image.paste(rendered, (45, 0))
    return image


register('textstay', TextStay)
