/*
 * contrib/btree_gist/btree_rangetypes.c
 */
#include "postgres.h"
#include "utils/rangetypes.h"
#include "utils/sortsupport.h"

static int	range_gist_cmp(Datum a, Datum b, SortSupport ssup);
PG_FUNCTION_INFO_V1(gbt_range_gist_sortsupport);

/*
 * GiST sortsupport comparator for ranges.
 *
 * Operates solely on the lower bounds of the ranges, comparing them using
 * range_cmp_bounds().
 * Empty ranges are sorted before non-empty ones.
 */
static int
range_gist_cmp(Datum a, Datum b, SortSupport ssup)
{
	RangeType *range_a = DatumGetRangeTypeP(a);
	RangeType *range_b = DatumGetRangeTypeP(b);
	TypeCacheEntry *typcache = ssup->ssup_extra;
	RangeBound	lower1,
				lower2;
	RangeBound	upper1,
				upper2;
	bool		empty1,
				empty2;
	int			result;

	if (typcache == NULL) {
		Assert(RangeTypeGetOid(range_a) == RangeTypeGetOid(range_b));
		typcache = lookup_type_cache(RangeTypeGetOid(range_a), TYPECACHE_RANGE_INFO);

		/*
		 * Cache the range info between calls to avoid having to call
		 * lookup_type_cache() for each comparison.
		 */
		ssup->ssup_extra = typcache;
	}

	range_deserialize(typcache, range_a, &lower1, &upper1, &empty1);
	range_deserialize(typcache, range_b, &lower2, &upper2, &empty2);

	/* For b-tree use, empty ranges sort before all else */
	if (empty1 && empty2)
		result = 0;
	else if (empty1)
		result = -1;
	else if (empty2)
		result = 1;
	else
		result = range_cmp_bounds(typcache, &lower1, &lower2);

	if ((Datum) range_a != a)
		pfree(range_a);

	if ((Datum) range_b != b)
		pfree(range_b);

	return result;
}

/*
 * Sort support routine for fast GiST index build by sorting.
 */
Datum
gbt_range_gist_sortsupport(PG_FUNCTION_ARGS)
{
	SortSupport	ssup = (SortSupport) PG_GETARG_POINTER(0);

	ssup->comparator = range_gist_cmp;
	ssup->ssup_extra = NULL;

	PG_RETURN_VOID();
}
