#include <Storages/System/StorageSystemMutations.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeArray.h>
#include <DataStreams/OneBlockInputStream.h>
#include <Storages/StorageReplicatedMergeTree.h>
#include <Storages/VirtualColumnUtils.h>
#include <Databases/IDatabase.h>


namespace DB
{

StorageSystemMutations::StorageSystemMutations(const std::string & name_)
    : name(name_)
{
    setColumns(ColumnsDescription({
        { "database",                             std::make_shared<DataTypeString>()      },
        { "table",                                std::make_shared<DataTypeString>()      },
        { "mutation_id",                          std::make_shared<DataTypeString>()      },
        { "command",                              std::make_shared<DataTypeString>()      },
        { "create_time",                          std::make_shared<DataTypeDateTime>()    },
        { "block_numbers.partition_id",           std::make_shared<DataTypeArray>(
                                                      std::make_shared<DataTypeString>()) },
        { "block_numbers.number",                 std::make_shared<DataTypeArray>(
                                                      std::make_shared<DataTypeInt64>())  },
        { "parts_to_do",                          std::make_shared<DataTypeInt64>()      },
        { "is_done",                              std::make_shared<DataTypeUInt8>()      },
    }));
}


BlockInputStreams StorageSystemMutations::read(
    const Names & column_names,
    const SelectQueryInfo & query_info,
    const Context & context,
    QueryProcessingStage::Enum & processed_stage,
    const size_t /*max_block_size*/,
    const unsigned /*num_streams*/)
{
    check(column_names);
    processed_stage = QueryProcessingStage::FetchColumns;

    /// Collect a set of replicated tables.
    std::map<String, std::map<String, StoragePtr>> replicated_tables;
    for (const auto & db : context.getDatabases())
        for (auto iterator = db.second->getIterator(context); iterator->isValid(); iterator->next())
            if (dynamic_cast<const StorageReplicatedMergeTree *>(iterator->table().get()))
                replicated_tables[db.first][iterator->name()] = iterator->table();

    MutableColumnPtr col_database_mut = ColumnString::create();
    MutableColumnPtr col_table_mut = ColumnString::create();

    for (auto & db : replicated_tables)
    {
        for (auto & table : db.second)
        {
            col_database_mut->insert(db.first);
            col_table_mut->insert(table.first);
        }
    }

    ColumnPtr col_database = std::move(col_database_mut);
    ColumnPtr col_table = std::move(col_table_mut);

    /// Determine what tables are needed by the conditions in the query.
    {
        Block filtered_block
        {
            { col_database, std::make_shared<DataTypeString>(), "database" },
            { col_table, std::make_shared<DataTypeString>(), "table" },
        };

        VirtualColumnUtils::filterBlockWithQuery(query_info.query, filtered_block, context);

        if (!filtered_block.rows())
            return BlockInputStreams();

        col_database = filtered_block.getByName("database").column;
        col_table = filtered_block.getByName("table").column;
    }

    MutableColumns res_columns = getSampleBlock().cloneEmptyColumns();
    for (size_t i_storage = 0; i_storage < col_database->size(); ++i_storage)
    {
        auto database = (*col_database)[i_storage].safeGet<String>();
        auto table = (*col_table)[i_storage].safeGet<String>();

        std::vector<MergeTreeMutationStatus> states =
            dynamic_cast<StorageReplicatedMergeTree &>(*replicated_tables[database][table])
                .getMutationsStatus();

        for (const MergeTreeMutationStatus & status : states)
        {
            Array block_partition_ids;
            block_partition_ids.reserve(status.block_numbers.size());
            Array block_numbers;
            block_numbers.reserve(status.block_numbers.size());
            for (const auto & pair : status.block_numbers)
            {
                block_partition_ids.emplace_back(pair.first);
                block_numbers.emplace_back(pair.second);
            }

            size_t col_num = 0;
            res_columns[col_num++]->insert(database);
            res_columns[col_num++]->insert(table);

            res_columns[col_num++]->insert(status.id);
            res_columns[col_num++]->insert(status.command);
            res_columns[col_num++]->insert(UInt64(status.create_time));
            res_columns[col_num++]->insert(block_partition_ids);
            res_columns[col_num++]->insert(block_numbers);
            res_columns[col_num++]->insert(status.parts_to_do);
            res_columns[col_num++]->insert(UInt64(status.is_done));
        }
    }

    Block res = getSampleBlock().cloneEmpty();
    for (size_t i_col = 0; i_col < res.columns(); ++i_col)
        res.getByPosition(i_col).column = std::move(res_columns[i_col]);

    return BlockInputStreams(1, std::make_shared<OneBlockInputStream>(res));
}

}
