import slangpy as spy
import numpy as np
import matplotlib.pyplot as plt
import numpy.typing as npt

FFT_DIMS_X : int = 512
FFT_DIMS_Y : int = 512
CASCADE_COUNT : int = 4

def create_checkerboard_pattern(dims, block_size):
    y, x = np.ogrid[:dims, :dims]
    pattern = ((x // block_size) + (y // block_size)) % 2
    return pattern.astype(np.float16) * 2.0 - 1.0

def create_gaussian_circle_pattern(dims, sigma):
    y, x = np.ogrid[-dims//2 : dims//2, -dims//2 : dims//2]
    r_sq = x * x + y * y
    pattern = np.exp(-r_sq / (2 * sigma**2))
    return pattern.astype(np.float16)

def plot_ifft_error_heatmaps(ref_slice : npt.NDArray[np.float16], radix2_slice : npt.NDArray[np.float16], radix4_slice : npt.NDArray[np.float16]):
    err_radix2_ref = np.abs(radix2_slice - ref_slice)
    err_radix4_ref = np.abs(radix4_slice - ref_slice)
    err_radix2_radix4 = np.abs(radix2_slice - radix4_slice)
    # 2. Setup the figure layout
    fig, axes = plt.subplots(1, 3, figsize=(18, 5))
    fig.suptitle("IFFT Implementation Error Heatmaps", fontsize=16)

    # Define a shared color scale for the reference comparisons so they are 1:1 comparable
    vmax = max(np.max(err_radix2_ref), np.max(err_radix4_ref))

    # 3. Plot Radix-2 vs Reference
    # using origin='lower' to match standard GPU texture coordinates (0,0 at bottom left)
    im0 = axes[0].imshow(err_radix2_ref, cmap='magma', origin='lower', vmin=0, vmax=vmax)
    axes[0].set_title(f"Radix-2 (f16 mode) vs Numpy (Max: {np.max(err_radix2_ref):.4f})")
    fig.colorbar(im0, ax=axes[0], fraction=0.046, pad=0.04)

    # 4. Plot Radix-4 vs Reference
    im1 = axes[1].imshow(err_radix4_ref, cmap='magma', origin='lower', vmin=0, vmax=vmax)
    axes[1].set_title(f"Radix-4 vs Numpy (Max: {np.max(err_radix4_ref):.4f})")
    fig.colorbar(im1, ax=axes[1], fraction=0.046, pad=0.04)

    # 5. Plot Radix-2 vs Radix-4 (Auto-scaled color mapping)
    im2 = axes[2].imshow(err_radix2_radix4, cmap='viridis', origin='lower')
    axes[2].set_title(f"Radix-2 vs Radix-4 (Max: {np.max(err_radix2_radix4):.4f})")
    fig.colorbar(im2, ax=axes[2], fraction=0.046, pad=0.04)

    plt.tight_layout()
    plt.show()

def compute_reference_fft(spatial_data: np.ndarray) -> np.ndarray:
    """
    Takes a (Cascade, Y, X, 4) array of spatial data and returns 
    the frequency domain representation in the same packed format.
    """
    # 1. Upcast to float32 to avoid precision issues during CPU FFT
    data_f32 = spatial_data.astype(np.float32)
    
    # 2. Unpack channels into complex numbers
    # Since channels 1 and 3 are currently 0.0, this safely creates 
    # a purely real complex number (e.g., checkerboard + 0j)
    complex_1 = data_f32[..., 0] + 1j * data_f32[..., 1]
    complex_2 = data_f32[..., 2] + 1j * data_f32[..., 3]
    
    # 3. Run the Forward 2D FFT
    # axes=(-2, -1) ensures we only FFT across the Y and X dimensions, ignoring cascades
    fft_1 = np.fft.fft2(complex_1, axes=(-2, -1))
    fft_2 = np.fft.fft2(complex_2, axes=(-2, -1))
    
    # 4. Pack the resulting frequencies back into half4 format (Real, Imag, Real, Imag)
    frequency_data = np.zeros_like(spatial_data)
    frequency_data[..., 0] = fft_1.real.astype(np.float16)
    frequency_data[..., 1] = fft_1.imag.astype(np.float16)
    frequency_data[..., 2] = fft_2.real.astype(np.float16)
    frequency_data[..., 3] = fft_2.imag.astype(np.float16)
    
    return frequency_data

def compute_reference_ifft(input_data : np.ndarray):
    data_f32 = input_data.astype(np.float32)
    # c1 = x + i*y
    complex_1 = data_f32[..., 0] + 1j * data_f32[..., 1]
    # c2 = z + i*w
    complex_2 = data_f32[..., 2] + 1j * data_f32[..., 3]
    
    # 2. Run the 2D Inverse FFT
    # axes=(-2, -1) tells Numpy to only FFT across the FFT_DIMS (Y, X)
    # leaving the CASCADE_COUNT dimension isolated.
    ifft_1 = np.fft.ifft2(complex_1, axes=(-2, -1))
    ifft_2 = np.fft.ifft2(complex_2, axes=(-2, -1))
    
    # 3. Pack back into half4 format (Real, Imag, Real, Imag)
    output_data = np.zeros_like(input_data, dtype=np.float16)
    output_data[..., 0] = ifft_1.real.astype(np.float16)
    output_data[..., 1] = ifft_1.imag.astype(np.float16)
    output_data[..., 2] = ifft_2.real.astype(np.float16)
    output_data[..., 3] = ifft_2.imag.astype(np.float16)
    
    return output_data

if __name__ == "__main__":
    slangDevice = spy.Device()
    module = slangDevice.load_module("slang_shaders/OceanFft.slang")

    slangLinkOptions = spy.SlangLinkOptions()
    slangLinkOptions.floating_point_mode = spy.SlangFloatingPointMode.fast
    slangLinkOptions.optimization = spy.SlangOptimizationLevel.maximal

    # create a dict of entrypoint names to linked programs
    shader_programs = {entry_point.name: slangDevice.link_program([module], [entry_point], slangLinkOptions)
                    for entry_point in module.entry_points}

    # each program has a reflection cursor: this makes it way easier to query the resources and parameters
    # and bind/set data on them
    shader_layouts = {name: program.layout for name, program in shader_programs.items()}

    # now use dict of programs to create a dict of entrypoint names to compute pipelines
    kernels = {name: slangDevice.create_compute_kernel(program=program)
                    for name, program in shader_programs.items()}

    checkerboard_2d = create_checkerboard_pattern(FFT_DIMS_X, 128) / 10000.0
    gaussian_circle_2d = create_gaussian_circle_pattern(FFT_DIMS_X, FFT_DIMS_X / 4) / np.log(FFT_DIMS_X*FFT_DIMS_Y)
    input_checkerboard = np.zeros((CASCADE_COUNT, FFT_DIMS_Y, FFT_DIMS_X, 4), dtype=np.float16)
    input_checkerboard[..., 0] = checkerboard_2d  # Real part of X channel
    input_checkerboard[..., 2] = checkerboard_2d  # Real part of Z channel
    input_gaussian = np.zeros((CASCADE_COUNT, FFT_DIMS_Y, FFT_DIMS_X, 4), dtype=np.float16)
    input_gaussian[..., 0] = gaussian_circle_2d  # Real part of X channel
    input_gaussian[..., 2] = gaussian_circle_2d  # Real part of Z channel

    frequency_checkerboard = compute_reference_fft(input_checkerboard)
    frequency_gaussian = compute_reference_fft(input_gaussian)

    gpu_test_data = frequency_gaussian.flatten()  # Use checkerboard for testing

    #test_data = np.zeros((CASCADE_COUNT, FFT_DIMS_Y, FFT_DIMS_X, 4), dtype=np.float16)
    # dirac delta
    #np.random.seed(34)
    #test_data[:] = np.random.uniform(-1.0, 1.0, test_data.shape).astype(np.float16)
    #y, x = np.ogrid[-FFT_DIMS_Y//2 : FFT_DIMS_Y//2, -FFT_DIMS_X//2 : FFT_DIMS_X//2]
    #distance = np.sqrt(x*x + y*y)
    #distance[distance == 0] = 1.0 # Prevent divide-by-zero at the DC component
    #pink_mask = (1.0 / distance).astype(np.float16)
    # Multiply your uniform noise by the mask across all channels/cascades
    #pink_test_data = test_data * pink_mask[np.newaxis, ..., np.newaxis]
    # flatten to create gpu buffer input
    #gpu_test_data = pink_test_data.flatten()

    radix2_buffer = slangDevice.create_buffer(
        element_count=FFT_DIMS_X * FFT_DIMS_Y * CASCADE_COUNT,
        resource_type_layout=kernels["Radix2_IFFT"].reflection.Radix2_IFFT_Input,
        usage=spy.BufferUsage.unordered_access,
        data=gpu_test_data)
    kernels["Radix2_IFFT"].dispatch(
        thread_count=[FFT_DIMS_X, FFT_DIMS_Y, CASCADE_COUNT],
        vars={"Radix2_IFFT_Transpose": False, "Radix2_IFFT_Input": radix2_buffer})
    kernels["Radix2_IFFT"].dispatch(
        thread_count=[FFT_DIMS_X, FFT_DIMS_Y, CASCADE_COUNT],
        vars={"Radix2_IFFT_Transpose": True, "Radix2_IFFT_Input": radix2_buffer})
    cpu_result = compute_reference_ifft(frequency_gaussian)

    gpu_output_readback = radix2_buffer.to_numpy().view(np.float16)
    gpu_output_reshaped = gpu_output_readback.reshape((CASCADE_COUNT, FFT_DIMS_Y, FFT_DIMS_X, 4))
    
    is_valid = np.allclose(cpu_result, gpu_output_reshaped, atol=1e-3, rtol=1e-3)
    if is_valid:
        print("Radix-2 IFFT test passed!")
    else:
        max_error = np.max(np.abs(cpu_result - gpu_output_reshaped))
        print(f"Radix-2 IFFT test failed! Max error: {max_error}")

    # Now test Radix-4 IFFT
    radix4_buffer = slangDevice.create_buffer(
        element_count=FFT_DIMS_X * FFT_DIMS_Y * CASCADE_COUNT,
        resource_type_layout=kernels["Radix4_IFFT"].reflection.Radix4_IFFT_Input,
        usage=spy.BufferUsage.unordered_access,
        data=gpu_test_data)
    kernels["Radix4_IFFT"].dispatch(
        thread_count=[FFT_DIMS_X, FFT_DIMS_Y, CASCADE_COUNT],
        vars={"Radix4_IFFT_Transpose": False, "Radix4_IFFT_BonusWaveOpsRound" : False, "Radix4_IFFT_Input": radix4_buffer})
    kernels["Radix4_IFFT"].dispatch(
        thread_count=[FFT_DIMS_X, FFT_DIMS_Y, CASCADE_COUNT],
        vars={"Radix4_IFFT_Transpose": True, "Radix4_IFFT_BonusWaveOpsRound" : False, "Radix4_IFFT_Input": radix4_buffer})

    gpu_output_readback_r4 = radix4_buffer.to_numpy().view(np.float16)
    gpu_output_reshaped_r4 = gpu_output_readback_r4.reshape((CASCADE_COUNT, FFT_DIMS_Y, FFT_DIMS_X, 4))
    is_valid = np.allclose(cpu_result, gpu_output_reshaped_r4, atol=1e-3, rtol=1e-3)
    if is_valid:
        print("Radix-4 IFFT test passed!")
    else:
        max_error = np.max(np.abs(cpu_result - gpu_output_reshaped_r4))
        print(f"Radix-4 IFFT test failed! Max error: {max_error}")

    ref_2d = cpu_result[0, :, :, 0]
    r2_2d = gpu_output_reshaped[0, :, :, 0]
    r4_2d = gpu_output_reshaped_r4[0, :, :, 0]

    plot_ifft_error_heatmaps(ref_2d, r2_2d, r4_2d)

    plt.figure(figsize=(12, 8))
    plt.suptitle(f"IDFT Roundtrip Comparison, N={FFT_DIMS_X}, Cascade=0")
    plt.subplot(2, 2, 1)
    plt.title("Gaussian Input Signal")
    plt.imshow(gaussian_circle_2d, cmap='coolwarm', origin='lower')
    plt.subplot(2, 2, 2)
    plt.title("Gaussian Reference CPU IDFT")
    plt.imshow(ref_2d, cmap='coolwarm', origin='lower')
    plt.subplot(2, 2, 3)
    plt.title("Gaussian Radix-2 GPU IDFT")
    plt.imshow(r2_2d, cmap='coolwarm', origin='lower')
    plt.subplot(2, 2, 4)
    plt.title("Gaussian Radix-4 GPU IDFT")
    plt.imshow(r4_2d, cmap='coolwarm', origin='lower')
    plt.show()
    
    