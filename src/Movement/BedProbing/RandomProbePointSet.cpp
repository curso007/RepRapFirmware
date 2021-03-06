/*
 * RandomProbePointSet.cpp
 *
 *  Created on: 6 May 2017
 *      Author: David
 */

#include "RandomProbePointSet.h"
#include "RepRap.h"
#include "Platform.h"

#if SUPPORT_OBJECT_MODEL

// Object model table and functions
// Note: if using GCC version 7.3.1 20180622 and lambda functions are used in this table, you must compile this file with option -std=gnu++17.
// Otherwise the table will be allocated in RAM instead of flash, which wastes too much RAM.

// Macro to build a standard lambda function that includes the necessary type conversions
#define OBJECT_MODEL_FUNC(...) OBJECT_MODEL_FUNC_BODY(RandomProbePointSet, __VA_ARGS__)

constexpr ObjectModelTableEntry RandomProbePointSet::objectModelTable[] =
{
	// These entries must be in alphabetical order
	{ "numPointsProbed", OBJECT_MODEL_FUNC((int32_t)self->numBedCompensationPoints), ObjectModelEntryFlags::none }
};

constexpr uint8_t RandomProbePointSet::objectModelTableDescriptor[] = { 1, 1 };

DEFINE_GET_OBJECT_MODEL_TABLE(RandomProbePointSet)

#endif

RandomProbePointSet::RandomProbePointSet() noexcept : numBedCompensationPoints(0)
{
	for (size_t point = 0; point < MaxProbePoints; point++)
	{
		probePointSet[point] = unset;
		zBedProbePoints[point] = 0.0;		// so that the M122 report looks tidy
	}
}

// Record the X and Y coordinates of a probe point
void RandomProbePointSet::SetXYBedProbePoint(size_t index, float x, float y) noexcept
{
	xBedProbePoints[index] = x;
	yBedProbePoints[index] = y;
	probePointSet[index] |= xySet;
}

// Record the Z coordinate of a probe point
void RandomProbePointSet::SetZBedProbePoint(size_t index, float z, bool wasXyCorrected, bool wasError) noexcept
{
	zBedProbePoints[index] = z;
	probePointSet[index] |= zSet;

	if (wasXyCorrected)
	{
		probePointSet[index] |= xyCorrected;
	}
	else
	{
		probePointSet[index] &= ~xyCorrected;
	}

	if (wasError)
	{
		probePointSet[index] |= probeError;
	}
	else
	{
		probePointSet[index] &= ~probeError;
	}
}

size_t RandomProbePointSet::NumberOfProbePoints() const noexcept
{
	for (size_t i = 0; i < MaxProbePoints; i++)
	{
		if ((probePointSet[i] & (xySet | zSet)) != (xySet | zSet))
		{
			return i;
		}
	}
	return MaxProbePoints;
}

// Clear out the Z heights so that we don't re-use old points
void RandomProbePointSet::ClearProbeHeights() noexcept
{
	for (size_t i = 0; i < MaxProbePoints; ++i)
	{
		probePointSet[i] &= ~zSet;
	}
}

// Set the bed transform, returning true if error
bool RandomProbePointSet::SetProbedBedEquation(size_t numPoints, const StringRef& reply) noexcept
{
	if (!GoodProbePointOrdering(numPoints))
	{
		reply.printf("Probe points P0 to P%u must be in clockwise order starting near minimum X and Y", min<unsigned int>(numPoints, 4) - 1);
		return true;
	}

	switch(numPoints)
	{
	case 3:
		/*
		 * Transform to a plane
		 */
		{
			const float x10 = xBedProbePoints[1] - xBedProbePoints[0];
			const float y10 = yBedProbePoints[1] - yBedProbePoints[0];
			const float z10 = zBedProbePoints[1] - zBedProbePoints[0];
			const float x20 = xBedProbePoints[2] - xBedProbePoints[0];
			const float y20 = yBedProbePoints[2] - yBedProbePoints[0];
			const float z20 = zBedProbePoints[2] - zBedProbePoints[0];
			const float a = y10 * z20 - z10 * y20;
			const float b = z10 * x20 - x10 * z20;
			const float c = x10 * y20 - y10 * x20;
			const float d = -(xBedProbePoints[1] * a + yBedProbePoints[1] * b + zBedProbePoints[1] * c);
			aX = -a / c;
			aY = -b / c;
			aC = -d / c;
		}
		break;

	case 4:
		/*
		 * Transform to a ruled-surface quadratic.  The corner points for interpolation are indexed:
		 *
		 *   ^  [1]      [2]
		 *   |
		 *   Y
		 *   |
		 *   |  [0]      [3]
		 *      -----X---->
		 *
		 *   These are the scaling factors to apply to x and y coordinates to get them into the
		 *   unit interval [0, 1].
		 */
		xRectangle = 1.0 / (xBedProbePoints[3] - xBedProbePoints[0]);
		yRectangle = 1.0 / (yBedProbePoints[1] - yBedProbePoints[0]);
		break;

	default:
		reply.printf("Bed calibration: %d points provided but only 3 and 4 points supported", numPoints);
		return true;
	}

	numBedCompensationPoints = numPoints;

	reprap.GetPlatform().Message(WarningMessage,
		"3/4-point bed compensation is deprecated and will be removed in a future firmware release. Please use G29 mesh bed compensation instead.\n");

	// Report what points the bed equation fits
	reply.copy("Bed equation fits points");
	for (size_t point = 0; point < numPoints; point++)
	{
		reply.catf(" [%.1f, %.1f, %.3f]", (double)xBedProbePoints[point], (double)yBedProbePoints[point], (double)zBedProbePoints[point]);
	}
	return false;
}

// Compute the interpolated height error at the specified point
float RandomProbePointSet::GetInterpolatedHeightError(float x, float y) const noexcept
{
	switch(numBedCompensationPoints)
	{
	case 0:
	default:
		return 0.0;

	case 3:
		return aX * x + aY * y + aC;

	case 4:
		return SecondDegreeTransformZ(x, y);
	}
}

// Check whether the specified set of points has been successfully defined and probed
bool RandomProbePointSet::GoodProbePoints(size_t numPoints) const noexcept
{
	for (size_t i = 0; i < numPoints; ++i)
	{
		if ((probePointSet[i] & (xySet | zSet | probeError)) != (xySet | zSet))
		{
			return false;
		}
	}
	return true;
}

// Check that the probe points are in the right order
bool RandomProbePointSet::GoodProbePointOrdering(size_t numPoints) const noexcept
{
	if (numPoints >= 2 && yBedProbePoints[1] <= yBedProbePoints[0])
	{
		return false;
	}
	if (numPoints >= 3 && xBedProbePoints[2] <= xBedProbePoints[1])
	{
		return false;
	}
	if (numPoints >= 4 && yBedProbePoints[3] >= yBedProbePoints[2])
	{
		return false;
	}
	if (numPoints >= 4 && xBedProbePoints[0] >= xBedProbePoints[3])
	{
		return false;
	}
	return true;
}

// Print out the probe heights and any errors
void RandomProbePointSet::ReportProbeHeights(size_t numPoints, const StringRef& reply) const noexcept
{
	reply.copy("G32 bed probe heights:");
	float sum = 0.0;
	float sumOfSquares = 0.0;
	for (size_t i = 0; i < numPoints; ++i)
	{
		if ((probePointSet[i] & (xySet | zSet)) != (xySet | zSet))
		{
			reply.cat(" not set");
		}
		else if ((probePointSet[i] & probeError) != 0)
		{
			reply.cat(" probing failed");
		}
		else
		{
			reply.catf(" %.3f", (double)zBedProbePoints[i]);
			sum += zBedProbePoints[i];
			sumOfSquares += fsquare(zBedProbePoints[i]);
		}
	}
	const float mean = sum/numPoints;
	// In the following, if there is only 1 point we may try to take the square root of a negative number due to rounding error, hence the 'max' call
	const float stdDev = sqrtf(max<float>(sumOfSquares/numPoints - fsquare(mean), 0.0));
	reply.catf(", mean %.3f, deviation from mean %.3f", (double)mean, (double)stdDev);
}

/*
 * Transform to a ruled-surface quadratic.  The corner points for interpolation are indexed:
 *
 *   ^  [1]      [2]
 *   |
 *   Y
 *   |
 *   |  [0]      [3]
 *      -----X---->
 *
 *   The values of x and y are transformed to put them in the interval [0, 1].
 */
float RandomProbePointSet::SecondDegreeTransformZ(float x, float y) const noexcept
{
	x = (x - xBedProbePoints[0])*xRectangle;
	y = (y - yBedProbePoints[0])*yRectangle;
	return (1.0 - x)*(1.0 - y)*zBedProbePoints[0] + x*(1.0 - y)*zBedProbePoints[3] + (1.0 - x)*y*zBedProbePoints[1] + x*y*zBedProbePoints[2];
}

void RandomProbePointSet::DebugPrint(size_t numPoints) const noexcept
{
	debugPrintf("Z probe offsets:");
	float sum = 0.0;
	float sumOfSquares = 0.0;
	for (size_t i = 0; i < numPoints; ++i)
	{
		debugPrintf(" %.3f", (double)zBedProbePoints[i]);
		sum += zBedProbePoints[i];
		sumOfSquares += fsquare(zBedProbePoints[i]);
	}
	const float mean = sum/numPoints;
	debugPrintf(", mean %.3f, deviation from mean %.3f\n", (double)mean, (double)sqrtf(sumOfSquares/numPoints - fsquare(mean)));
}

// End
