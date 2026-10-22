module CmShared;

// The imported module of CrossModuleCookTest. It sits outside the folder of the kernel and outside that folder's
// parent, so only --include-dir can find it. An `__include` fragment is not used here: ddColor needs one, and the
// import is what this test is about.

public static const float k_Bias = 0.5;

public float AddBias(float x)
{
    return x + k_Bias;
}
