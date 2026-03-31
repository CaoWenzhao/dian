import os
import torch
import torchvision
import torchvision.transforms as transforms
from torch.utils.data import DataLoader, Subset
from esp_ppq import QuantizationSettingFactory, QuantizationSetting   # 加上 QuantizationSetting
from esp_ppq.api import espdl_quantize_onnx, get_target_platform
from typing import Iterable, Tuple, List

# ====================== 配置参数 ======================
BATCH_SIZE = 32
INPUT_SHAPE = [1, 28, 28]          # MNIST: 1通道, 28x28
DEVICE = "cpu"                     # 'cuda' 或 'cpu'
TARGET = "esp32s3"                 # 'esp32s3' 或 'esp32p4'
NUM_OF_BITS = 8
ONNX_PATH = "mnist_model_example.onnx"  # 请确保已有 MNIST 的 ONNX 模型
ESPDL_MODEL_PATH = "./models/mnist.espdl"
CALIB_DIR = "./mnist_calib"        # 临时目录，实际直接从 dataset 加载

# ====================== 可选：量化优化设置 ======================
def quant_setting_mnist(
    onnx_path: str,
    optim_quant_method: List[str] = None,
) -> Tuple[QuantizationSetting, str]:
    """针对 MNIST 的量化设置，一般无需复杂优化"""
    quant_setting = QuantizationSettingFactory.espdl_setting()
    if optim_quant_method is not None:
        if "MixedPrecision_quantization" in optim_quant_method:
            # 示例：指定某些层使用 16-bit（需知道实际层名）
            # 这里留空，用户可根据需要添加
            pass
        elif "LayerwiseEqualization_quantization" in optim_quant_method:
            # 对于 MNIST 通常不需要，若需要可开启
            quant_setting.equalization = True
            quant_setting.equalization_setting.iterations = 4
            quant_setting.equalization_setting.value_threshold = 0.4
            quant_setting.equalization_setting.opt_level = 2
            quant_setting.equalization_setting.interested_layers = None
        else:
            raise ValueError("Unsupported optimization method")
    return quant_setting, onnx_path

def collate_fn(batch):
    """将 batch 中的图像堆叠成张量并转移到指定设备"""
    images = torch.stack([item[0] for item in batch])
    return images.to(DEVICE)

# ====================== 校准数据集加载 ======================
def get_mnist_calibration_dataloader(sample_count=1024):
    """从 MNIST 训练集前 sample_count 张图片中创建校准 DataLoader"""
    transform = transforms.Compose([
        transforms.ToTensor(),
        transforms.Normalize((0.1307,), (0.3081,))  # MNIST 均值和标准差
    ])
    full_dataset = torchvision.datasets.MNIST(
        root='./data', train=True, download=True, transform=transform
    )
    # 取前 sample_count 张用于校准（足够获得稳定量化参数）
    subset = Subset(full_dataset, indices=list(range(min(sample_count, len(full_dataset)))))
    dataloader = DataLoader(
        subset, batch_size=BATCH_SIZE, shuffle=False,
        num_workers=2, pin_memory=False, collate_fn=collate_fn
    )
    return dataloader

# ====================== 评估函数 ======================
def evaluate_ppq_module_with_mnist(quant_model, test_batchsize=128, device='cpu'):
    """在 MNIST 测试集上评估量化模型的准确率"""
    transform = transforms.Compose([
        transforms.ToTensor(),
        transforms.Normalize((0.1307,), (0.3081,))
    ])
    test_dataset = torchvision.datasets.MNIST(
        root='./data', train=False, download=True, transform=transform
    )
    test_loader = DataLoader(test_dataset, batch_size=test_batchsize, shuffle=False)

    correct = 0
    total = 0
    with torch.no_grad():
        for images, labels in test_loader:
            images = images.to(device)
            labels = labels.to(device)
            # 量化模型前向推理（注意输入形状需匹配）
            outputs = quant_model(images)
            _, predicted = torch.max(outputs, 1)
            total += labels.size(0)
            correct += (predicted == labels).sum().item()
    accuracy = 100.0 * correct / total
    print(f"MNIST Test Accuracy: {accuracy:.2f}%")
    return accuracy

# ====================== 主流程 ======================
if __name__ == "__main__":
    # 1. 准备校准数据加载器
    calib_dataloader = get_mnist_calibration_dataloader(sample_count=1024)
    print(f"Calibration dataloader size: {len(calib_dataloader)} batches")

    # 2. 配置量化参数（此处不使用额外优化，直接使用默认）
    quant_setting, onnx_path = quant_setting_mnist(ONNX_PATH, optim_quant_method=None)

    # 3. 执行量化
    quant_ppq_graph = espdl_quantize_onnx(
        onnx_import_file=onnx_path,
        espdl_export_file=ESPDL_MODEL_PATH,
        calib_dataloader=calib_dataloader,
        calib_steps=32,                     # 使用32个batch，每个batch 32张图，共1024张
        input_shape=[1] + INPUT_SHAPE,      # [1,1,28,28]
        target=TARGET,
        num_of_bits=NUM_OF_BITS,
        collate_fn=collate_fn,
        setting=quant_setting,
        device=DEVICE,
        error_report=True,
        skip_export=False,
        export_test_values=False,
        verbose=1,
    )

    # 4. 评估量化后模型精度
    evaluate_ppq_module_with_mnist(quant_ppq_graph, test_batchsize=128, device=DEVICE)