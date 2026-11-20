# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: Apache-2.0
import pytest
from pytest_embedded import Dut
from pytest_embedded_idf.utils import idf_parametrize


@pytest.mark.mosaico_nand
@idf_parametrize('target', ['esp32s31'], indirect=['target'])
def test_mosaico_nand_auto(dut: Dut) -> None:
    dut.expect('Press ENTER to see the list of tests')
    dut.write('[auto]')
    dut.expect(r'\d+ Tests 0 Failures 0 Ignored', timeout=300)
