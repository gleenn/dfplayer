# -*- coding: utf-8 -*-
# Licensed under The MIT License
#
# Controls TCL controller.

from PIL import Image

from .stats import Stats
from .tcl_layout import TclLayout
from .tcl_renderer_py import TclRenderer as TclPyImpl
from .renderer_cc import Layout as TclCcLayout
from .renderer_cc import TclRenderer as TclCcImpl
from .renderer_cc import AdjustableTime as TclCcTime
from .util import get_time_millis

class TclRenderer(object):
  """To use this class:
       renderer = TclRenderer(controller_id, width, height, 'layout.dxf', 2.4)
       # 'image_colors' has tuples with 4 RGBA bytes for each pixel,
       # with 'height' number of sequential rows, each row having
       # length of 'width' pixels.
       renderer.send_frame(image_data)
  """

  def __init__(self, fps, use_cc_impl, enable_net, test_mode=False):
    self._fps = fps
    self._use_cc_impl = use_cc_impl
    self._enable_net = enable_net
    self._test_mode = test_mode
    self._hdr_mode = 2  # Saturation only
    self._widths = {}
    self._heights = {}

  def add_controller(self, controller_id, width, height, gamma):
    self._widths[controller_id] = width
    self._heights[controller_id] = height
    layout_file = 'dfplayer/layout%d.dxf' % controller_id
    layout_src = TclLayout(layout_file, width - 1, height - 1)
    if self._use_cc_impl:
      layout = TclCcLayout()
      for s in layout_src.get_strands():
        for c in s.get_coords():
          layout.AddCoord(s.get_id(), c[0], c[1])
      self._renderer = TclCcImpl.GetInstance()
      self._renderer.AddController(controller_id, width, height, layout, gamma)
    else:
      self._renderer = TclPyImpl(controller_id, width, height, layout_src)
      self.set_gamma(gamma)
      if not self._test_mode:
        self._renderer.connect()

  def lock_controllers(self):
    if self._use_cc_impl:
      self._renderer.LockControllers()
      self._frame_send_duration = self._renderer.GetFrameSendDuration()
      self._renderer.SetHdrMode(self._hdr_mode)
      if not self._test_mode:
        self._renderer.StartMessageLoop(self._fps, self._enable_net)
    else:
      self._frame_send_duration = self._renderer.get_send_duration_ms()
    self._frame_delays = []
    self._frame_delays_clear_time = get_time_millis()

  def set_gamma(self, gamma):
    # 1.0 is uncorrected gamma, which is perceived as "too bright"
    # in the middle. 2.4 is a good starting point. Changing this value
    # affects mid-range pixels - higher values produce dimmer pixels.
    self.set_gamma_ranges((0, 255, gamma), (0, 255, gamma), (0, 255, gamma))

  def set_gamma_ranges(self, r, g, b):
    if self._use_cc_impl:
      self._renderer.SetGammaRanges(
          r[0], r[1], r[2], g[0], g[1], g[2], b[0], b[1], b[2])
    else:
      self._renderer.set_gamma_ranges(r, g, b)

  def disable_reset(self):
    if self._use_cc_impl:
      self._renderer.SetAutoResetAfterNoDataMs(0)

  def has_scheduling_support(self):
    return self._use_cc_impl

  def get_and_clear_last_image(self, controller):
    if not self._use_cc_impl:
      print 'get_and_clear_last_image not supported'
      return None
    img_data = self._renderer.GetAndClearLastImage(controller)
    if not img_data or len(img_data) == 0:
      return None
    return Image.fromstring(
        'RGBA', (self._widths[controller], self._heights[controller]),
        img_data)

  def get_and_clear_last_led_image(self, controller):
    if not self._use_cc_impl:
      print 'get_and_clear_last_led_image not supported'
      return None
    img_data = self._renderer.GetAndClearLastLedImage(controller)
    if not img_data or len(img_data) == 0:
      return None
    return Image.fromstring(
        'RGBA', (self._widths[controller], self._heights[controller]),
        img_data)

  def get_last_image_id(self, controller):
    if not self._use_cc_impl:
      return 0
    return self._renderer.GetLastImageId(controller)

  def get_send_duration_ms(self):
    return self._frame_send_duration

  def get_queue_size(self):
    if self._use_cc_impl:
      return self._renderer.GetQueueSize()
    else:
      return -1

  def get_init_status(self):
    if self._use_cc_impl:
      return self._renderer.GetInitStatus()
    else:
      return 'Not Provided'

  def reset_image_queue(self):
    if self._use_cc_impl:
      self._renderer.ResetImageQueue()

  def send_frame(self, controller, image, id, delay_ms):
    if self._use_cc_impl:
      time = TclCcTime()
      time.AddMillis(delay_ms - self._frame_send_duration)
      self._renderer.ScheduleImageAt(
          controller, image.tostring(), image.size[0], image.size[1],
          0, 0, image.size[0], image.size[1], 0, 2, 0, id, time, True)
    else:
      self._renderer.send_frame(list(image.getdata()), get_time_millis())

  def set_effect_image(self, controller, image, mirror):
    if self._use_cc_impl:
      if image:
        self._renderer.SetEffectImage(
            controller, image.tostring(),
            image.size[0], image.size[1], 2 if mirror else 1)
      else:
        self._renderer.SetEffectImage(controller, '', 0, 0, 2)

  def toggle_hdr_mode(self):
    if not self._use_cc_impl:
      return
    self._hdr_mode += 1
    self._hdr_mode %= 4
    self._renderer.SetHdrMode(self._hdr_mode)

  def get_hdr_mode(self):
    return self._hdr_mode

  def get_frame_data_for_test(self, controller, image):
    if self._use_cc_impl:
      return self._renderer.GetFrameDataForTest(
          controller, image.tostring())
    else:
      return self._renderer.get_frame_data_for_test(list(image.getdata()))

  def get_and_clear_frame_delays(self):
    self._populate_frame_delays()
    result = self._frame_delays
    duration = get_time_millis() - self._frame_delays_clear_time
    self._frame_delays = []
    self._frame_delays_clear_time = get_time_millis()
    return (duration, result)

  def _populate_frame_delays(self):
    if self._use_cc_impl:
      for d in self._renderer.GetAndClearFrameDelays():
        self._frame_delays.append(d)
    else:
      for d in self._renderer.get_and_clear_frame_delays():
        # Pretent that sending had no delays as we currently
        # have no way to compensate for that.
        d -= self._frame_send_duration
        self._frame_delays.append(d if d > 0 else 0)

