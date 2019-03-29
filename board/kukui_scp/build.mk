# -*- makefile -*-
# Copyright 2018 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# Board specific files build
#
CHIP:=mt_scp
CHIP_VARIANT:=mt8183

board-y=board.o
board-$(HAS_TASK_VDEC_SERVICE)+=vdec.o
board-$(HAS_TASK_VENC_SERVICE)+=venc.o
