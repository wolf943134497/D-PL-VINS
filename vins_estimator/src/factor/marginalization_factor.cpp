#include "marginalization_factor.h"


#include <glog/logging.h>

#include "../utility/logging.h"
#include "pose_local_parameterization.h"



void ResidualBlockInfo::Evaluate()
{
    residuals.resize(cost_function->num_residuals());

    std::vector<int> block_sizes = cost_function->parameter_block_sizes();
    raw_jacobians = new double *[block_sizes.size()];
    jacobians.resize(block_sizes.size());

    for (int i = 0; i < static_cast<int>(block_sizes.size()); i ++)
    {
        jacobians[i].resize(cost_function->num_residuals(), block_sizes[i]);
        raw_jacobians[i] = jacobians[i].data();
        // dim += block_sizes[i] == 7 ? 6 : block_sizes[i];
    }
    cost_function->Evaluate(parameter_blocks.data(), residuals.data(), raw_jacobians);

    //std::vector<int> tmp_idx(block_sizes.size());
    //Eigen::MatrixXd tmp(dim, dim);
    //for (int i = 0; i < static_cast<int>(parameter_blocks.size()); i++)
    //{
    //    int size_i = localSize(block_sizes[i]);
    //    Eigen::MatrixXd jacobian_i = jacobians[i].leftCols(size_i);
    //    for (int j = 0, sub_idx = 0; j < static_cast<int>(parameter_blocks.size()); sub_idx += block_sizes[j] == 7 ? 6 : block_sizes[j], j++)
    //    {
    //        int size_j = localSize(block_sizes[j]);
    //        Eigen::MatrixXd jacobian_j = jacobians[j].leftCols(size_j);
    //        tmp_idx[j] = sub_idx;
    //        tmp.block(tmp_idx[i], tmp_idx[j], size_i, size_j) = jacobian_i.transpose() * jacobian_j;
    //    }
    //}
    //Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> saes(tmp);
    //std::cout << saes.eigenvalues() << std::endl;
    //ROS_ASSERT(saes.eigenvalues().minCoeff() >= -1e-6);

    if (loss_function)
    {
        double residual_scaling_, alpha_sq_norm_;

        double sq_norm, rho[3];

        sq_norm = residuals.squaredNorm();
        loss_function->Evaluate(sq_norm, rho);
        // printf("sq_norm: %f, rho[0]: %f, rho[1]: %f, rho[2]: %f\n", sq_norm, rho[0], rho[1], rho[2]);

        double sqrt_rho1_ = sqrt(rho[1]);

        if ((sq_norm == 0.0) || (rho[2] <= 0.0))
        {
            residual_scaling_ = sqrt_rho1_;
            alpha_sq_norm_ = 0.0;
        }
        else
        {
            const double D = 1.0 + 2.0 * sq_norm * rho[2] / rho[1];
            const double alpha = 1.0 - sqrt(D);
            residual_scaling_ = sqrt_rho1_ / (1 - alpha);
            alpha_sq_norm_ = alpha / sq_norm;
        }

        for (int i = 0; i < static_cast<int>(parameter_blocks.size()); i ++)
        {
            jacobians[i] = sqrt_rho1_ * (jacobians[i] -
                           alpha_sq_norm_ * residuals * (residuals.transpose() * jacobians[i]));
        }

        residuals *= residual_scaling_;
    }
}

MarginalizationInfo::~MarginalizationInfo()
{
    // ROS_WARN("release marginlizationinfo");
    
    for (auto it = parameter_block_data.begin(); it != parameter_block_data.end(); ++ it)
        delete it->second;          // zlc添加：RGBD这里是删除数组 []

    for (int i = 0; i < (int)factors.size(); i ++)
    {

        delete[] factors[i]->raw_jacobians;
        
        delete factors[i]->cost_function;

        delete factors[i];
    }
}

void MarginalizationInfo::addResidualBlockInfo(ResidualBlockInfo* residual_block_info)
{
    factors.emplace_back(residual_block_info);

    std::vector<double *> &parameter_blocks = residual_block_info->parameter_blocks;                       // 这个残差模块对应的优化参数
    std::vector<int> parameter_block_sizes = residual_block_info->cost_function->parameter_block_sizes();  // 每个参数是多少维的

    for (int i = 0; i < static_cast<int>(residual_block_info->parameter_blocks.size()); i ++)    // 遍历每个参数
    {
        double* addr = parameter_blocks[i];                // 参数首地址
        int size = parameter_block_sizes[i];               // 参数维数
        parameter_block_size[reinterpret_cast<long>(addr)] = size;
    }

    for (int i = 0; i < static_cast<int>(residual_block_info->drop_set.size()); i ++)
    {
        double* addr = parameter_blocks[residual_block_info->drop_set[i]];
        parameter_block_idx[reinterpret_cast<long>(addr)] = 0;     // 先把要drop掉的参数，存进parameter_block_idx，这些参数在H矩阵里的索引位置初始化为0
    }
}

void MarginalizationInfo::preMarginalize()
{
    for (auto it : factors)
    {
        it->Evaluate();

        std::vector<int> block_sizes = it->cost_function->parameter_block_sizes();
        for (int i = 0; i < static_cast<int>(block_sizes.size()); i ++)
        {
            long addr = reinterpret_cast<long>(it->parameter_blocks[i]);
            int size = block_sizes[i];
            if (parameter_block_data.find(addr) == parameter_block_data.end())   // 如果prior param里还没有存这个参数，那就存
            {
                double* data = new double[size];
                memcpy(data, it->parameter_blocks[i], sizeof(double) * size);
                parameter_block_data[addr] = data;
            }
        }
    }
}

int MarginalizationInfo::localSize(int size) const
{
    return size == 7 ? 6 : size;
}

int MarginalizationInfo::globalSize(int size) const
{
    return size == 6 ? 7 : size;
}


/**
 * @brief 分线程构造Ax=b，   构建Hessien矩阵A = J^T * J，b=J^T * r
 *
 */
void* ThreadsConstructA(void* threadsstruct)
{
    ThreadsStruct* p = ((ThreadsStruct*)threadsstruct);
    // 遍历这么多分配过来的任务
    for (auto it : p->sub_factors)
    {
        // 遍历参数块
        for (int i = 0; i < static_cast<int>(it->parameter_blocks.size()); i ++)
        {
            int idx_i = p->parameter_block_idx[reinterpret_cast<long>(it->parameter_blocks[i])];    // 在大矩阵中的id，也就是落座的位置
            int size_i = p->parameter_block_size[reinterpret_cast<long>(it->parameter_blocks[i])];
            // 确保是local size
            if (size_i == 7)
                size_i = 6;
            Eigen::MatrixXd jacobian_i = it->jacobians[i].leftCols(size_i);    

            /*
            Eigen::MatrixXd jacobian_i = it->jacobians[i].leftCols(size_i);
            if (size_i == 7)
            {
                size_i = 6;
                Eigen::Matrix<double, 7, 6, Eigen::RowMajor> Jglobal_local;
                PoseLocalParameterization::Global2LocalJacobian(it->parameter_blocks[i],
                                                                &Jglobal_local);
                jacobian_i = it->jacobians[i] * Jglobal_local;
            }
            */
            for (int j = i; j < static_cast<int>(it->parameter_blocks.size()); j ++)
            {
                int idx_j  = p->parameter_block_idx[reinterpret_cast<long>(it->parameter_blocks[j])];
                int size_j = p->parameter_block_size[reinterpret_cast<long>(it->parameter_blocks[j])];


                if (size_j == 7)
                    size_j = 6;
                Eigen::MatrixXd jacobian_j = it->jacobians[j].leftCols(size_j);

                /*
                Eigen::MatrixXd jacobian_j;
                jacobian_j = it->jacobians[j].leftCols(size_j);
                if (size_j == 7)
                {
                    size_j = 6;
                    Eigen::Matrix<double, 7, 6, Eigen::RowMajor> Jglobal_local;
                    PoseLocalParameterization::Global2LocalJacobian(
                            it->parameter_blocks[j], &Jglobal_local);
                    jacobian_j = it->jacobians[j] * Jglobal_local;
                }
                 */

                if (i == j)
                    p->A.block(idx_i, idx_j, size_i, size_j) +=
                            jacobian_i.transpose() * jacobian_j;
                else
                {
                    p->A.block(idx_i, idx_j, size_i, size_j) += jacobian_i.transpose() * jacobian_j;
                    p->A.block(idx_j, idx_i, size_j, size_i) = p->A.block(idx_i, idx_j, size_i, size_j).transpose();
                }
            }
            p->b.segment(idx_i, size_i) += jacobian_i.transpose() * it->residuals;
        }
    }
    return threadsstruct;
}


/**
 * @brief 边缘化处理，并将结果转换成残差和雅克比的形式
 *
 */
void MarginalizationInfo::marginalize()
{
    int pos = 0;

    // parameter_block_idx_key是各个待边缘化参数块地址   value预设都是0
    int marg_pose_size = 0;

    std::map<int, int, std::greater<int> > marg_pose_index_size; // 记住需要 marg 掉的 pose 的 index;
    for (auto& it : parameter_block_idx)    // parameter_block_idx一开始只保存了marg掉的参数,  it.first: 参数地址
    {

        if(localSize(parameter_block_size[it.first]) > 4)  // pose size is 6, velocity and bias size is 9
        {
            marg_pose_index_size.insert(std::make_pair(pos,localSize(parameter_block_size[it.first])));
            marg_pose_size += localSize(parameter_block_size[it.first]);
        }
        
        it.second = pos;                    // parameter_block_idx 存好每个参数块 在Hessien矩阵A里的位置索引
        pos += localSize(parameter_block_size[it.first]);
    }

    // 总共待边缘化的参数块总大小（不是个数）
    m = pos;   // 要marg掉的参数维数， 包括marg掉的帧 + marg掉的特征
    // 其他参数块
    for (const auto& it : parameter_block_size)    // 遍历所有的参数块
    {
        // 之前 parameter_block_idx 里只存着 marg 掉的参数在hessian里的索引
        if (parameter_block_idx.find(it.first) == parameter_block_idx.end())   // 如果这些参数没有添加进parameter_block_idx，把他们也添加进去
        {
            parameter_block_idx[it.first] = pos;      // 存好每个参数块 在Hessien矩阵A里的位置索引
            pos += localSize(it.second);
        }
    }

    n = pos - m;   // n 非marg参数的维数, 这个主要是其他关键帧,以及外参数的维度

    ROS_INFO("marginalization, pos: %d, m: %d, n: %d, size: %d", pos, m, n, (int)parameter_block_idx.size());
    // std::cout << "marginalization_factor.cpp/marginalize(): marginalization, pos: " << pos << "m: " << m << "n: " << n << "size: " << (int)parameter_block_idx.size() << std::endl;

    TicToc t_summing;
    Eigen::MatrixXd A(pos, pos);   // Hessien矩阵的维度
    Eigen::VectorXd b(pos);
    A.setZero();
    b.setZero();

    /*

    // 遍历 和 marg pose 有关的所有 factor
    for (auto it : factors)
    {
        // it->parameter_blocks： 和这个factor有关的所有参数块
        for (int i = 0; i < static_cast<int>(it->parameter_blocks.size()); i++)    ROS_DEBUG("marginalization, pos: %d, m: %d, n: %d, size: %d", pos, m, n, (int)parameter_block_idx.size());
    std::cout << "marginalization
        {
            int idx_i = parameter_block_idx[reinterpret_cast<long>(it->parameter_blocks[i])];
            int size_i = localSize(parameter_block_size[reinterpret_cast<long>(it->parameter_blocks[i])]);
            Eigen::MatrixXd jacobian_i = it->jacobians[i].leftCols(size_i);
            for (int j = i; j < static_cast<int>(it->parameter_blocks.size()); j ++)
            {
                // 把 雅克比 取出来， 放到对应的位置
                int idx_j = parameter_block_idx[reinterpret_cast<long>(it->parameter_blocks[j])];
                int size_j = localSize(parameter_block_size[reinterpret_cast<long>(it->parameter_blocks[j])]);
                Eigen::MatrixXd jacobian_j = it->jacobians[j].leftCols(size_j);
                if (i == j)
                    A.block(idx_i, idx_j, size_i, size_j) += jacobian_i.transpose() * jacobian_j;
                else
                {
                    A.block(idx_i, idx_j, size_i, size_j) += jacobian_i.transpose() * jacobian_j;
                    A.block(idx_j, idx_i, size_j, size_i) = A.block(idx_i, idx_j, size_i, size_j).transpose();
                }
            }
            b.segment(idx_i, size_i) += jacobian_i.transpose() * it->residuals;
        }
    }
    ROS_INFO("summing up costs %f ms", t_summing.toc());
    */

    // multi thread  往A矩阵和b矩阵中填东西，利用多线程加速
    TicToc t_thread_summing;
    pthread_t tids[NUM_THREADS];
    ThreadsStruct threadsstruct[NUM_THREADS];
    int i = 0;
    for (auto it : factors)
    {
        threadsstruct[i].sub_factors.push_back(it);     // 每个线程均匀分配任务
        i ++;
        i = i % NUM_THREADS;
    }
    // 每个线程构造一个A矩阵和b矩阵，最后大家加起来
    for (int i = 0; i < NUM_THREADS; i ++)
    {
        TicToc zero_matrix;
        // 所以A矩阵和b矩阵大小一样，预设都是0
        threadsstruct[i].A = Eigen::MatrixXd::Zero(pos,pos);
        threadsstruct[i].b = Eigen::VectorXd::Zero(pos);
        // 多线程访问会带来冲突，因此每个线程备份一下要查询的map
        threadsstruct[i].parameter_block_size = parameter_block_size;       // 大小
        threadsstruct[i].parameter_block_idx = parameter_block_idx;         // 索引
        // 产生若干线程
        int ret = pthread_create( &tids[i], NULL, ThreadsConstructA ,(void*)&(threadsstruct[i]));
        if (ret != 0)
        {
            ROS_WARN("pthread_create error");
            // std::cout << "marginalization_factor.cpp/marginalize(): pthread_create error!" << std::endl;
            ROS_BREAK();
        }
    }
    for( int i = NUM_THREADS - 1; i >= 0; i --)
    {
        // 等待各个线程完成各自的任务
        pthread_join( tids[i], NULL );
        // 把各个子模块拼起来，就是最终的 Hx = g的矩阵了
        A += threadsstruct[i].A;
        b += threadsstruct[i].b;
    }
    // ROS_DEBUG("thread summing up costs %f ms", t_thread_summing.toc());
    // ROS_INFO("A diff %f , b diff %f ", (A - tmp_A).sum(), (b - tmp_b).sum());


///////////////////// marg accletor code////////////////////////////////////////////////////////////////////////////////
    // marg landmark firstly
    // TicToc tmarg_l;
    int m1 = m-marg_pose_size;
    int n1 = n + marg_pose_size;
    if( m1 > 0 )
    {
        Eigen::MatrixXd A_reorder(A);
        Eigen::VectorXd b_reorder(b);
    
        for (auto iter: marg_pose_index_size)
        {
            int idx = iter.first; // 循环marg的时候，index会变化, 所以我们的marg_pose_size是降序排列的，先移动index大的
            int size = iter.second;

            // 将 row i 移动矩阵最下面
            Eigen::MatrixXd temp_rows =  A_reorder.block(idx,0, size,pos);
            A_reorder.block(idx,0,m-idx-size,pos) = A_reorder.block(idx+size,0,m-idx-size,pos);
            A_reorder.block(m-size,0, size,pos) = temp_rows;

            // 将 col i 移动矩阵最右边
            Eigen::MatrixXd temp_cols = A_reorder.block(0,idx,pos,size);
            A_reorder.block(0,idx,pos,m-idx-size) = A_reorder.block(0,idx+size,pos,m-idx-size);
            A_reorder.block(0, m-size,pos,size) = temp_cols;

            Eigen::VectorXd temp_b = b_reorder.segment(idx,size);
            b_reorder.segment(idx, m -idx-size) =  b_reorder.segment(idx+size,m-idx-size);
            b_reorder.segment(m-size,size) = temp_b;
        }

        Eigen::MatrixXd Amm1 = A_reorder.block(0,0,m1,m1);
        Eigen::VectorXd bmm1 = b_reorder.segment(0, m1);
       
        // TicToc t_inv;
        // Eigen::MatrixXd Amm_inv1 = Amm1.diagonal().asDiagonal().inverse();    // when only use point feature
        Eigen::MatrixXd Amm_inv1 = Amm1.inverse();
        // ROS_INFO("invese Amm costs %f ms", tmarg_l.toc());
        
        Eigen::MatrixXd Amr1 = A_reorder.block(0, m1, m1, n1);
        Eigen::MatrixXd Arm1 = A_reorder.block(m1, 0, n1, m1);
        Eigen::MatrixXd Arr1 = A_reorder.block(m1, m1, n1, n1);
        Eigen::VectorXd brr1 = b_reorder.segment(m1, n1);
        // TicToc t_mult;
        Eigen::MatrixXd tempA = Arm1 * Amm_inv1;
        A = Arr1 - tempA * Amr1;
        b = brr1 - tempA * bmm1;
    }

    // then marg pose
    // TicToc tmarg_pose;
    int m2 = m - m1;
    int n2 = n;

    // Amm矩阵的构建是为了保证其正定性
    Eigen::MatrixXd Amm = 0.5 * (A.block(0, 0, m2, m2) +
                                 A.block(0, 0, m2, m2).transpose());
    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> saes(Amm);                     // 特征值分解

    // ROS_ASSERT_MSG(saes.eigenvalues().minCoeff() >= -1e-4, "min eigenvalue %f", saes.eigenvalues().minCoeff());
    // 一个逆矩阵的特征值是原矩阵的倒数，特征向量相同  select类似c++中 ？ : 运算符
    // 利用特征值取逆来构造其逆矩阵
    Eigen::MatrixXd Amm_inv = saes.eigenvectors()
            * Eigen::VectorXd((saes.eigenvalues().array() > eps).select(saes.eigenvalues().array().inverse(), 0)).asDiagonal()
            * saes.eigenvectors().transpose();
    // printf("error1: %f\n", (Amm * Amm_inv - Eigen::MatrixXd::Identity(m, m)).sum());

    Eigen::VectorXd bmm = b.segment(0, m2);                                     // 待边缘化的大小
    Eigen::MatrixXd Amr = A.block(0, m2, m2, n2);     // 对应的四块矩阵
    Eigen::MatrixXd Arm = A.block(m2, 0, n2, m2);
    Eigen::MatrixXd Arr = A.block(m2, m2, n2, n2);
    Eigen::VectorXd brr = b.segment(m2, n2);                                    // 剩下的参数
    Eigen::MatrixXd tempB = Arm * Amm_inv;
    A = Arr - tempB * Amr;
    b = brr - tempB * bmm;
    // ROS_INFO("marg pose costs %f ms", tmarg_pose.toc());
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


    // 这个地方根据Ax = b => JT*J = -JT * e
    // 对A做特征值分解 A = V * S * VT,其中Ｓ是特征值构成的对角矩阵
    // 所以J = S^(1/2) * VT , 这样JT * J = (S^(1/2) * VT)T * S^(1/2) * VT = V * S^(1/2)T *  S^(1/2) * VT = V * S * VT(对角矩阵的转置等于其本身)
    // e = -(JT)-1 * b = - (S^-(1/2) * V^-1) * b = - (S^-(1/2) * VT) * b

    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> saes2(A);
    // 对A矩阵取逆
    Eigen::VectorXd S = Eigen::VectorXd((saes2.eigenvalues().array() > eps).select(saes2.eigenvalues().array(), 0));
    Eigen::VectorXd S_inv = Eigen::VectorXd((saes2.eigenvalues().array() > eps).select(saes2.eigenvalues().array().inverse(), 0));

    Eigen::VectorXd S_sqrt = S.cwiseSqrt();             // 这个求得就是 S^(1/2)，不过这里是向量还不是矩阵
    Eigen::VectorXd S_inv_sqrt = S_inv.cwiseSqrt();

    // 边缘化为了实现对剩下参数块的约束，为了便于一起优化，就抽象成了残差和雅克比的形式，这样也形成了一种残差约束
    linearized_jacobians = S_sqrt.asDiagonal() * saes2.eigenvectors().transpose();
    linearized_residuals = S_inv_sqrt.asDiagonal() * saes2.eigenvectors().transpose() * b;

    //std::cout << A << std::endl
    //          << std::endl;
    //std::cout << linearized_jacobians << std::endl;
    //printf("error2: %f %f\n", (linearized_jacobians.transpose() * linearized_jacobians - A).sum(),
    //      (linearized_jacobians.transpose() * linearized_residuals - b).sum());
}

// void MarginalizationInfo::marginalize()
// {
//    int pos = 0;
//    for (auto &it : parameter_block_idx)    // parameter_block_idx一开始只保存了marg掉的参数,  it.first: 参数地址
//    {
//        it.second = pos;                    // parameter_block_idx 存好每个参数块 在Hessien矩阵A里的位置索引
//        pos += localSize(parameter_block_size[it.first]);
//    }

//    m = pos;   // 要marg掉的参数维数， 包括marg掉的帧 + marg掉的特征

//    for (const auto &it : parameter_block_size)    //遍历所有的参数块
//    {
//        // 之前 parameter_block_idx 里只存着 marg 掉的参数在hessian里的索引
//        if (parameter_block_idx.find(it.first) == parameter_block_idx.end())   // 如果这些参数没有添加进parameter_block_idx，把他们也添加进去
//        {
//            parameter_block_idx[it.first] = pos;      // 存好每个参数块 在Hessien矩阵A里的位置索引
//            pos += localSize(it.second);
//        }
//    }

//    n = pos - m;   // n 非marg参数的维数, 这个主要是其他关键帧,以及外参数的维度

//    ROS_INFO("marginalization, pos: %d, m: %d, n: %d, size: %d", pos, m, n, (int)parameter_block_idx.size());

//    TicToc t_summing;
//    Eigen::MatrixXd A(pos, pos);   // Hessien矩阵的维度
//    Eigen::VectorXd b(pos);
//    A.setZero();
//    b.setZero();


//    TicToc t_thread_summing;
//    pthread_t tids[NUM_THREADS];
//    ThreadsStruct threadsstruct[NUM_THREADS];
//    int i = 0;
//    for (auto it : factors)
//    {
//        threadsstruct[i].sub_factors.push_back(it);
//        i++;
//        i = i % NUM_THREADS;
//    }
//    for (int i = 0; i < NUM_THREADS; i++)
//    {
//        TicToc zero_matrix;
//        threadsstruct[i].A = Eigen::MatrixXd::Zero(pos,pos);
//        threadsstruct[i].b = Eigen::VectorXd::Zero(pos);
//        threadsstruct[i].parameter_block_size = parameter_block_size;
//        threadsstruct[i].parameter_block_idx = parameter_block_idx;
//        int ret = pthread_create( &tids[i], NULL, ThreadsConstructA ,(void*)&(threadsstruct[i]));
//        if (ret != 0)
//        {
//            ROS_WARN("pthread_create error");
//            ROS_BREAK();
//        }
//    }
//    for( int i = NUM_THREADS - 1; i >= 0; i--)
//    {
//        pthread_join( tids[i], NULL );
//        A += threadsstruct[i].A;
//        b += threadsstruct[i].b;
//    }

//    //TODO
//    Eigen::MatrixXd Amm = 0.5 * (A.block(0, 0, m, m) + A.block(0, 0, m, m).transpose());
//    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> saes(Amm);

//    //ROS_ASSERT_MSG(saes.eigenvalues().minCoeff() >= -1e-4, "min eigenvalue %f", saes.eigenvalues().minCoeff());
//    //std::cout<<"svd:   "<<saes.eigenvalues().maxCoeff() << std::endl;
//    Eigen::MatrixXd Amm_inv = saes.eigenvectors() * Eigen::VectorXd((saes.eigenvalues().array() > eps).select(saes.eigenvalues().array().inverse(), 0)).asDiagonal() * saes.eigenvectors().transpose();
//    //printf("error1: %f\n", (Amm * Amm_inv - Eigen::MatrixXd::Identity(m, m)).sum());

//    Eigen::VectorXd bmm = b.segment(0, m);
//    Eigen::MatrixXd Amr = A.block(0, m, m, n);
//    Eigen::MatrixXd Arm = A.block(m, 0, n, m);
//    Eigen::MatrixXd Arr = A.block(m, m, n, n);
//    Eigen::VectorXd brr = b.segment(m, n);
//    A = Arr - Arm * Amm_inv * Amr;       // n*n 维， 这里只有关键帧p，q,v,ba,bg 加上 外参数, 所以维度只和窗口内帧的多少有关，和特征多少无关， 这玩意会变得很稠密
//    b = brr - Arm * Amm_inv * bmm;

//    Eigen::SelfAdjointEigenSolver<Eigen::MatrixXd> saes2(A);
//    Eigen::VectorXd S = Eigen::VectorXd((saes2.eigenvalues().array() > eps).select(saes2.eigenvalues().array(), 0));
//    Eigen::VectorXd S_inv = Eigen::VectorXd((saes2.eigenvalues().array() > eps).select(saes2.eigenvalues().array().inverse(), 0));

//    Eigen::VectorXd S_sqrt = S.cwiseSqrt();
//    Eigen::VectorXd S_inv_sqrt = S_inv.cwiseSqrt();

//    linearized_jacobians = S_sqrt.asDiagonal() * saes2.eigenvectors().transpose();
//    linearized_residuals = S_inv_sqrt.asDiagonal() * saes2.eigenvectors().transpose() * b;

// }


std::vector<double *> MarginalizationInfo::getParameterBlocks(std::unordered_map<long, double *>& addr_shift)
{
    std::vector<double *> keep_block_addr;
    keep_block_size.clear();
    keep_block_idx.clear();
    keep_block_data.clear();

    for (const auto& it : parameter_block_idx)      // 遍历边缘化相关的每个参数块
    {
        if (it.second >= m)         // 如果是留下来的，说明后续会对其形成约束
        {
            keep_block_size.push_back(parameter_block_size[it.first]);      // 留下来的参数块大小 global size
            keep_block_idx.push_back(parameter_block_idx[it.first]);        // 留下来的在原向量中的排序
            keep_block_data.push_back(parameter_block_data[it.first]);      // 边缘化前各个参数块的值的备份
            keep_block_addr.push_back(addr_shift[it.first]);                // 对应的新地址
        }
    }
    // 留下来的边缘化后的参数块总大小
    sum_block_size = std::accumulate(std::begin(keep_block_size), std::end(keep_block_size), 0);

    return keep_block_addr;
}

MarginalizationFactor::MarginalizationFactor(MarginalizationInfo* _marginalization_info):marginalization_info(_marginalization_info)
{
    int cnt = 0;
    for (auto it : marginalization_info->keep_block_size)
    {
        mutable_parameter_block_sizes()->push_back(it);
        cnt += it;
    }
    // printf("residual size: %d, %d\n", cnt, n);
    // std::cout << "marginalization_factor.cpp/MarginalizationFactor(): residual size: " << cnt << std::endl;
    set_num_residuals(marginalization_info->n);
};

bool MarginalizationFactor::Evaluate(double const* const* parameters, double* residuals, double** jacobians) const
{
    // printf("internal addr,%d, %d\n", (int)parameter_block_sizes().size(), num_residuals());
    // for (int i = 0; i < static_cast<int>(keep_block_size.size()); i++)
    // {
    //     //printf("unsigned %x\n", reinterpret_cast<unsigned long>(parameters[i]));
    //     //printf("signed %x\n", reinterpret_cast<long>(parameters[i]));
    // printf("jacobian %x\n", reinterpret_cast<long>(jacobians));
    // printf("residual %x\n", reinterpret_cast<long>(residuals));
    // }
    int n = marginalization_info->n;       // 剩余误差的总维数，上一次边缘化保留的残差块的local size的和,也就是残差维数
    int m = marginalization_info->m;       // 上次边缘化的被margin的残差块总和
    Eigen::VectorXd dx(n);              // 用来存储残差

    // 遍历所有的剩下的有约束的残差块
    for (int i = 0; i < static_cast<int>(marginalization_info->keep_block_size.size()); i ++)
    {
        int size = marginalization_info->keep_block_size[i];
        int idx  = marginalization_info->keep_block_idx[i] - m;                                 // idx起点统一到0
        Eigen::VectorXd x  = Eigen::Map<const Eigen::VectorXd>(parameters[i], size);     // 当前参数块的值
        Eigen::VectorXd x0 = Eigen::Map<const Eigen::VectorXd>(marginalization_info->keep_block_data[i], size);   // marg留下的的prior估计，当时参数块的值
        if (size != 7)
            dx.segment(idx, size) = x - x0;  // bias , velocity etc.  不需要local param的直接做差
        else
        {
            // 位置直接做差
            dx.segment<3>(idx + 0) = x.head<3>() - x0.head<3>();
            // 旋转就是李代数做差
            dx.segment<3>(idx + 3) =
                    2.0 * Utility::positify(
                            Eigen::Quaterniond(x0(6), x0(3), x0(4), x0(5)).inverse()
                             * Eigen::Quaterniond(x(6), x(3), x(4), x(5))).vec();
            // 确保实部不大于0
            if (!((Eigen::Quaterniond(x0(6), x0(3), x0(4), x0(5)).inverse() *
                   Eigen::Quaterniond(x(6), x(3), x(4), x(5))).w() >= 0))
            {
                dx.segment<3>(idx + 3) =
                        2.0 * -Utility::positify(
                                Eigen::Quaterniond(x0(6), x0(3), x0(4), x0(5)).inverse()
                                *  Eigen::Quaterniond(x(6), x(3), x(4), x(5))).vec();
            }
        }
    }

    // 对先验部分的残差进行更新，这里使用的一阶雅科比进行更新 b = b + J * dx
    // 更新残差　边缘化后的先验误差 e = e0 + J * dx
    // 个人理解：根据FEJ．雅克比保持不变，但是残差随着优化会变化，因此下面不更新雅克比　只更新残差
    // 可以参考　https://blog.csdn.net/weixin_41394379/article/details/89975386
    Eigen::Map<Eigen::VectorXd>(residuals, n) = marginalization_info->linearized_residuals +
            marginalization_info->linearized_jacobians * dx;
    if (jacobians)
    {

        for (int i = 0; i < static_cast<int>(marginalization_info->keep_block_size.size()); i ++)
        {
            if (jacobians[i])
            {
                int size = marginalization_info->keep_block_size[i], local_size = marginalization_info->localSize(size);
                int idx = marginalization_info->keep_block_idx[i] - m;
                Eigen::Map<Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>> jacobian(jacobians[i], n, size);
                jacobian.setZero();
                jacobian.leftCols(local_size) =
                        marginalization_info->linearized_jacobians.middleCols(idx, local_size);

                /*  vins_plane中添加的部分
                if (size == 7)
                {
                    Eigen::Matrix<double, 6, 7, Eigen::RowMajor> jacobian_local2Global;
                    PoseLocalParameterization::Local2GlobalJacobian(parameters[i], &jacobian_local2Global);
                    jacobian.leftCols(size) =
                            marginalization_info->linearized_jacobians.middleCols(
                                    idx, local_size) * jacobian_local2Global;
                }
                */
            }
        }
    }
    return true;
}
