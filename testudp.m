clear; close all; clc;
delete(udpportfind("all"));
 
try
    % 1. 创建接收端 (监听端口25001)
    udp_recv = udpport("IPV4", 'LocalPort', 25001);
    fprintf('接收端: 监听端口 %d\n', udp_recv.LocalPort);
    
    % 2. 创建发送端 (本地端口25000，发送到25001)
    udp_send = udpport("IPV4", 'LocalPort', 25000);
    fprintf('发送端: 本地端口 %d\n', udp_send.LocalPort);
    fprintf('目标端口: 25001\n');
    
    % 配置超时
    udp_recv.Timeout = 5;
    
    % 清空接收缓冲区
    flush(udp_recv);
    
    % 测试数据
    test_messages = {
        '15678945631'
    };
    
    disp(' ');
    disp('开始测试...');
    
    for i = 1:length(test_messages)
        % 发送消息
        message = test_messages{i};
        fprintf('\n发送 %d: %s\n', i, message);
        
        % 发送数据
        write(udp_send, message, "string", "127.0.0.1", 25001);
        
        % 尝试接收
        pause(0.1);
        
        % 检查是否有数据到达
        if udp_recv.NumBytesAvailable > 0
            % 读取数据
            [data, src] = read(udp_recv, udp_recv.NumBytesAvailable, "string");
            fprintf('接收 %d: %s\n', i, data);
            fprintf('  来自: %s:%d\n', src.Address, src.Port);
        else
            fprintf('未收到响应\n');
        end
    end
    
    % 清理
    clear udp_send udp_recv;
    disp(' ');
    disp('测试完成！');
    
catch e
    fprintf('错误: %s\n', e.message);
    fprintf('端口绑定失败，尝试更换端口...\n');
    
    % 尝试另一组端口
    try
        udp_recv = udpport("IPV4", 'LocalPort', 25002);
        udp_send = udpport("IPV4", 'LocalPort', 25003);
        
        fprintf('新端口 - 接收端: %d，发送端: %d\n', 25002, 25003);
        fprintf('请重新运行测试代码\n');
    catch e2
        fprintf('再次失败: %s\n', e2.message);
    end
end