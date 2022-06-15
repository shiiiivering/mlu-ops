"""Test cosineEmbeddingLoss operator with multi-platform code link"""
from _pytest import mark
import numpy as np
import matplotlib
import pytest
from cosine_embedding_loss import DTYPES, KERNEL_NAME, TARGET_LIST
import bangpy
from evaluate_functions import evaluate
from bangpy.common import load_op_by_type

import random

np.set_printoptions(threshold = np.inf)

@pytest.mark.parametrize(
        "data_amount", 
        [
            2 ** 20 * 10,
            2 ** 30,
            2 ** 30 * 2,
            2 ** 30 * 4,
            2 ** 30 * 8
        ]
    )

@pytest.mark.parametrize(
        "data_width",
        [
            2 ** 5,
            2 ** 6,
            2 ** 7,
            2 ** 8,
            2 ** 9,
            2 ** 10,
            2 ** 11,
            2 ** 12,
            2 ** 13,
            2 ** 14,
            2 ** 15,
            2 ** 16,
            2 ** 17,
            2 ** 18,
            2 ** 19,
            ]
        )

@pytest.mark.parametrize(
        "dtype", DTYPES,
        )

def test_cosine_embedding_loss(target, data_amount, data_width, dtype):

    # float16为64位对齐
    if data_width == 32 and dtype == bangpy.float16:
        return
    f = load_op_by_type(KERNEL_NAME, dtype.name)

    evaluate(f, dtype, data_amount, data_width)


