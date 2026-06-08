package rcksum

/*
 * SPDX-FileCopyrightText: 2004,2005,2007,2009,2025,2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

import (
	"testing"
)

func buildTestRanges() blockRanges {
	return blockRanges{
		ranges: []blockIDPair{
			{Start: 0, End: 9},
			{Start: 11, End: 29},
			{Start: 40, End: 49},
		},
	}
}

func compareRanges(t *testing.T, got, expected []blockIDPair) {
	if len(got) != len(expected) {
		t.Fatalf("Expected %d ranges, got %d", len(expected), len(got))
	}

	for i := range expected {
		if got[i] != expected[i] {
			t.Errorf("Range %d mismatch: expected %+v, got %+v", i, expected[i], got[i])
		}
	}
}

func TestAddToRangesAlreadyIn(t *testing.T) {
	z := buildTestRanges()
	expected := z
	z.addToRanges(0)

	compareRanges(t, z.ranges, expected.ranges)
}

func TestAddToRangesFillHole(t *testing.T) {
	z := buildTestRanges()
	z.addToRanges(10)

	compareRanges(t, z.ranges, []blockIDPair{
		{Start: 0, End: 29},
		{Start: 40, End: 49},
	})
}

func TestAddToRangesExtendRangeBelow(t *testing.T) {
	z := buildTestRanges()
	z.addToRanges(39)

	compareRanges(t, z.ranges, []blockIDPair{
		{Start: 0, End: 9},
		{Start: 11, End: 29},
		{Start: 39, End: 49},
	})
}

func TestAddToRangesExtendRangeAbove(t *testing.T) {
	z := buildTestRanges()
	z.addToRanges(30)

	compareRanges(t, z.ranges, []blockIDPair{
		{Start: 0, End: 9},
		{Start: 11, End: 30},
		{Start: 40, End: 49},
	})
}

func TestAddToRangesNewRange(t *testing.T) {
	z := buildTestRanges()
	z.addToRanges(35)

	compareRanges(t, z.ranges, []blockIDPair{
		{Start: 0, End: 9},
		{Start: 11, End: 29},
		{Start: 35, End: 35},
		{Start: 40, End: 49},
	})
}
