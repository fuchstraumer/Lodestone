module KsTypes;

// The declaring module for every enum axis in the set. No axis lives here. A consumer module imports
// this module and declares an [ls_axis_enum] extern const of one of these enums.
//
// This placement is deliberate. The axis declaration and the enum definition sit in different modules,
// so the bootstrap must find the module that DECLARES the type, not the module where the axis constant
// was found. Today the reflection read records the second one, so this set proves that gap.
//
// The enum values are explicit and non-ascending, so a cook that assumed "case index == case value"
// makes wrong data. Reflection reads each real value, so the digit stays the declaration-order
// position while the stored value stays the real one.

public enum QualityTier
{
    Low = 3,
    Medium = 1,
    High = 7,
    Ultra = 2,
};

public enum ToneMapOperator
{
    Linear = 10,
    Reinhard = 40,
    Aces = 20,
    Filmic = 30,
};
