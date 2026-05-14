package rcksum

/*
 * SPDX-FileCopyrightText: 2004,2005,2007,2009,2025,2026 Colin Phipps <cph@moria.org.uk>
 *
 * SPDX-License-Identifier: Artistic-2.0
 */

// AI: copilot / claude code (if I remember correctly) conversion of zsync's ranges.c.

import "slices"

type blockIDPair struct {
	Start BlockID
	End   BlockID
}

type blockRanges struct {
	ranges    []blockIDPair
	gotBlocks int
}

// rangeBeforeBlock determines which existing range a block falls within
// Returns -1 if inside an existing range
// Returns 0 if before the 1st range
// Returns i if between range i-1 and range i
// Returns numRanges if after the last range
func (z *blockRanges) rangeBeforeBlock(x BlockID) int {
	min := 0
	max := len(z.ranges) - 1

	// Binary search
	for min <= max {
		r := (max + min) / 2

		if x > z.ranges[r].End {
			min = r + 1 // After range r
		} else if x < z.ranges[r].Start {
			max = r - 1 // Before range r
		} else {
			return -1 // In range r
		}
	}

	return min
}

// AddToRanges marks the given block as known, updating the ranges appropriately
func (z *blockRanges) addToRanges(x BlockID) {
	r := z.rangeBeforeBlock(x)

	if r == -1 {
		// Already have this block
		return
	}

	z.gotBlocks++

	// If between two ranges and exactly filling the hole, merge them
	if r > 0 && r < len(z.ranges) &&
		z.ranges[r-1].End == x-1 &&
		z.ranges[r].Start == x+1 {

		// Merge the two ranges
		z.ranges[r-1].End = z.ranges[r].End
		z.ranges = slices.Delete(z.ranges, r, r+1)
	} else if r > 0 && len(z.ranges) > 0 && z.ranges[r-1].End == x-1 {
		// Adjoining a range below, add to it
		z.ranges[r-1].End = x
	} else if r < len(z.ranges) && z.ranges[r].Start == x+1 {
		// Adjoining a range above, add to it
		z.ranges[r].Start = x
	} else {
		// New range for this block alone
		z.ranges = slices.Insert(z.ranges, r, blockIDPair{Start: x, End: x})
	}
}

// contains() checks if we already have the given block
func (z *blockRanges) contains(x BlockID) bool {
	return z.rangeBeforeBlock(x) == -1
}

// nextContainedAfter returns the block ID of the next known block after x.
// Or -1 if there are no known blocks after x.
func (z *blockRanges) nextContainedAfter(x BlockID) BlockID {

	r := z.rangeBeforeBlock(x)

	if r == -1 {
		// We already have this block, so find the end of this range
		// and return the block after the range
		for i := 0; i < len(z.ranges); i++ {
			if x >= z.ranges[i].Start && x <= z.ranges[i].End {
				return z.ranges[i].End + 1
			}
		}
	}

	// We don't have this block
	if r >= len(z.ranges) {
		// No more ranges
		return -1
	}

	// Return the start of the next range
	return z.ranges[r].Start
}

// missingBlocksBetween returns the ranges of blocks still needed between start and end.
// Returns pairs [start, end) of blocks not yet received
func (z *blockRanges) missingBlocksBetween(from, to BlockID) []blockIDPair {
	var result []blockIDPair

	// Clamp to valid range
	if from > to {
		return result
	}
	if from < 0 {
		from = 0
	}

	current := from

	for i := 0; i < len(z.ranges); i++ {
		rangeStart := z.ranges[i].Start
		rangeEnd := z.ranges[i].End

		// If current position is before this range, add the gap
		if current < rangeStart {
			gapEnd := rangeStart
			if gapEnd > to {
				gapEnd = to + 1
			}
			result = append(result, blockIDPair{Start: current, End: gapEnd - 1})
			current = rangeEnd + 1
		} else if current <= rangeEnd {
			// Current is within or at the end of this range
			current = rangeEnd + 1
		}
	}

	// If there's still space after the last range
	if current <= to {
		result = append(result, blockIDPair{Start: current, End: to})
	}

	return result
}
