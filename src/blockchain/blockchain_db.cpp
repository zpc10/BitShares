#include <bts/config.hpp>
#include <bts/blockchain/trx_validation_state.hpp>
#include <bts/blockchain/blockchain_db.hpp>
#include <bts/blockchain/blockchain_market_db.hpp>
#include <bts/blockchain/asset.hpp>
#include <leveldb/db.h>
#include <bts/db/level_pod_map.hpp>
#include <bts/db/level_map.hpp>
#include <fc/io/enum_type.hpp>
#include <fc/reflect/variant.hpp>
#include <fc/io/raw.hpp>
#include <fc/interprocess/mmap_struct.hpp>

#include <fc/filesystem.hpp>
#include <fc/log/logger.hpp>
#include <fc/io/json.hpp>

#include <algorithm>
#include <sstream>


    struct trx_stat
    {
       uint16_t trx_idx;
       bts::blockchain::trx_eval eval;
    };
    // sort with highest fees first
    bool operator < ( const trx_stat& a, const trx_stat& b )
    {
      return a.eval.fees.amount > b.eval.fees.amount;
    }
    FC_REFLECT( trx_stat, (trx_idx)(eval) )

namespace bts { namespace blockchain {
    namespace ldb = leveldb;
    namespace detail  
    { 
      /** a power of 2 array greater than the number of blocks per year */
      static const uint32_t DIVIDEND_HISTORY = uint32_t(1)<<17; // 131,072 > blocks per year
      typedef fc::array<fc::uint128,DIVIDEND_HISTORY>  asset_dividend_accumulator;
      struct dividend_table
      {
         fc::array<asset_dividend_accumulator, asset::count>  accumulators;
      };
      
      // TODO: .01 BTC update private members to use _member naming convention
      class blockchain_db_impl
      {
         public:
            blockchain_db_impl()
            :current_bitshare_supply(0){}

            //std::unique_ptr<ldb::DB> blk_id2num;  // maps blocks to unique IDs
            bts::db::level_map<fc::sha224,uint32_t>             blk_id2num;
            bts::db::level_map<uint160,trx_num>                 trx_id2num;
            bts::db::level_map<trx_num,meta_trx>                meta_trxs;
            bts::db::level_map<uint32_t,block>                  blocks;
            bts::db::level_map<uint32_t,std::vector<uint160> >  block_trxs; 

            market_db                                           _market_db;

            /** table that accumulates all dividends that should be paid
             * based upon coinage
             */
            fc::mmap_struct<dividend_table>                     dividend_acc_table;

            uint64_t                                            current_bitshare_supply;

            /** cache this information because it is required in many calculations  */
            trx_block                                           head_block;
            fc::sha224                                          head_block_id;
            // Dividend Table needs to be memory mapped

            void mark_spent( const output_reference& o, const trx_num& intrx, uint16_t in )
            {
               auto tid    = trx_id2num.fetch( o.trx_hash );
               meta_trx   mtrx   = meta_trxs.fetch( tid );
               FC_ASSERT( mtrx.meta_outputs.size() > o.output_idx );

               mtrx.meta_outputs[o.output_idx].trx_id    = intrx;
               mtrx.meta_outputs[o.output_idx].input_num = in;

               meta_trxs.store( tid, mtrx );
               remove_market_orders( o );
            }


            void remove_market_orders( const output_reference& o )
            {
               auto trx_out = get_output( o );
               if( trx_out.claim_func == claim_by_bid )
               {
                  auto cbb = trx_out.as<claim_by_bid_output>();
                  market_order order( cbb.ask_price, o );
                  _market_db.remove_bid( order );
               }

               if( trx_out.claim_func == claim_by_long )
               {
                  auto cbl = trx_out.as<claim_by_long_output>();
                  market_order order( cbl.ask_price, o );
                  _market_db.remove_ask( order );
               }
            }


            trx_output get_output( const output_reference& ref )
            { try {
               auto tid    = trx_id2num.fetch( ref.trx_hash );
               meta_trx   mtrx   = meta_trxs.fetch( tid );
               FC_ASSERT( mtrx.outputs.size() > ref.output_idx );
               return mtrx.outputs[ref.output_idx];
            } FC_RETHROW_EXCEPTIONS( warn, "", ("ref",ref) ) }
            
            /**
             *   Stores a transaction and updates the spent status of all 
             *   outputs doing one last check to make sure they are unspent.
             */
            void store( const signed_transaction& t, const trx_num& tn )
            {
               trx_id2num.store( t.id(), tn ); 
               meta_trxs.store( tn, meta_trx(t) );

               for( uint16_t i = 0; i < t.inputs.size(); ++i )
               {
                  mark_spent( t.inputs[i].output_ref, tn, i ); 
               }
               
               for( uint16_t i = 0; i < t.outputs.size(); ++i )
               {
                  if( t.outputs[i].claim_func == claim_by_bid )
                  {
                     claim_by_bid_output cbb = t.outputs[i].as<claim_by_bid_output>();
                     if( cbb.is_bid(t.outputs[i].unit) )
                     {
                        elog( "Insert Bid: ${bid}", ("bid",market_order(cbb.ask_price, output_reference( t.id(), i )) ) );
                        _market_db.insert_bid( market_order(cbb.ask_price, output_reference( t.id(), i )) );
                     }
                     else
                     {
                        elog( "Insert Ask: ${bid}", ("bid",market_order(cbb.ask_price, output_reference( t.id(), i )) ) );
                        _market_db.insert_ask( market_order(cbb.ask_price, output_reference( t.id(), i )) );
                     }
                  }
                  else if( t.outputs[i].claim_func == claim_by_long )
                  {
                    auto cbl = t.outputs[i].as<claim_by_long_output>();
                    elog( "Insert Short Ask: ${bid}", ("bid",market_order(cbl.ask_price, output_reference( t.id(), i )) ) );
                    _market_db.insert_ask( market_order(cbl.ask_price, output_reference( t.id(), i )) );
                  }
               }
            }

            void store( const trx_block& b )
            {
                for( uint16_t t = 0; t < b.trxs.size(); ++t )
                {
                   store( b.trxs[t], trx_num( b.block_num, t) );
                }
                head_block    = b;
                head_block_id = b.id();
            }

            /**
             *  Sets the dividend percent for bnum and the given unit to div_per
             *  Increments everything prior to bnum back one year by div_per
             */
            void accumulate_dividends_table( uint32_t bnum, uint64_t div_per, asset::type unit )
            {
               // TODO: what happens if power is lost while we are doing this, this operation is
               // not atomic so we must have some kind of log that allows us to rebuild this table
               // quickly when we start the program.  If we simply log the dividends in a file that
               // always grows like a jouralling DB then we can quickly detect and rebuild correupted
               // accumulation tables... perhaps we would be safer to always rebuild it on startup just
               // incase there was any corruption.
               
               fc::uint128 delta( 0, div_per );
               int32_t year_old = 0;
               if( bnum > BLOCKS_PER_YEAR ) 
               {
                  year_old = bnum - BLOCKS_PER_YEAR;
               }
               detail::asset_dividend_accumulator& ada = dividend_acc_table->accumulators.at( unit );
               ada.at( bnum % DIVIDEND_HISTORY ) = 0;
               for( int32_t i = bnum; i >= year_old; --i ) 
               {
                  uint32_t idx = i % DIVIDEND_HISTORY;
                  ada.at(idx) += delta;
                 // ilog( "div ${i}] ${per}", ("i",i)("per", asset(ada.at(idx), asset::bts) ) );
               }
            }
            fc::uint128 get_dividends( asset::type u, uint32_t blk_num )
            {
               return dividend_acc_table->accumulators.at( u ).at( blk_num % DIVIDEND_HISTORY );
            }

            void match_orders( std::vector<signed_transaction>& matched,  asset::type quote, asset::type base )
            { try {
                ilog( "match orders.." );
               auto bids = _market_db.get_bids( quote, base );
               auto asks = _market_db.get_asks( quote, base );
               ilog( "asks: ${asks}", ("asks",asks) );
               ilog( "bids: ${bids}", ("bids",bids) );

               fc::optional<trx_output>  ask_change;
               fc::optional<trx_output>  bid_change;
               fc::optional<trx_output>  cover_change;

               address                   bid_payout_address;
               fc::optional<asset>       bid_payout;

               asset                                cover_collat;       
               fc::optional<claim_by_cover_output>  cover_payout;


               signed_transaction market_trx;

               /** asks are sorted from low to high, so we start
                * with the lowest ask, and check to see if there are
                * any bids that are greaterthan or equal to the ask, if
                * there are then either the full bid or full ask will be
                * filled.  If the full bid is filled, then move on to the
                * next bid, and save the leftover ask.  If the left over
                * ask is filled, then move to the next ask.
                *
                * When there are no more pairs that can be matched, exit
                * the loop and any partial payouts are made.  
                */
               auto ask_itr = asks.begin();
               auto bid_itr = bids.rbegin();
               while( ask_itr != asks.end() &&
                      bid_itr != bids.rend() )
               { 
                  trx_output working_ask;
                  trx_output working_bid;

                  if( ask_change ) {  working_ask = *ask_change; }
                  else             {  working_ask = get_output( ask_itr->location );  }

                  if( bid_change ) {  working_bid = *bid_change; }
                  else             {  working_bid = get_output( bid_itr->location);   }

                  claim_by_bid_output bid_claim = working_bid.as<claim_by_bid_output>();

                  if( working_ask.claim_func == claim_by_long )
                  {
                     auto long_claim = working_ask.as<claim_by_long_output>();
                     if( long_claim.ask_price > bid_claim.ask_price )
                     {
                        break; // exit the while loop, no more trades can occur
                     }
                     asset bid_amount = working_bid.get_amount() * bid_claim.ask_price;
                     asset ask_amount = working_ask.get_amount() * long_claim.ask_price;
                     auto  trade_amount = std::min(bid_amount,ask_amount);
                     
                     FC_ASSERT( bid_amount.unit == ask_amount.unit );

                     ilog( "bid: ${b}   ask: ${a}", ("b",bid_amount)("a",ask_amount) );

                     asset bid_change_amount   = working_bid.get_amount();
                     bid_change_amount        -= trade_amount * bid_claim.ask_price;
                     ilog( "bid change.. ${c}", ("c",bid_change_amount) );
                     
                     asset ask_change_amount   = working_ask.get_amount();
                     ask_change_amount        -= trade_amount * long_claim.ask_price;
                     ilog( "ask change.. ${c}", ("c",ask_change_amount) );
                      /*
                     if( ask_change_amount != bid_change_amount  && ask_change_amount != asset(0,working_bid.unit) )
                     {
                       FC_ASSERT( !"At least one of the bid or ask should be completely filled", "", 
                                  ("ask_change_amount",ask_change_amount)("bid_change_amount",bid_change_amount) );
                     }
                     */

                     bid_payout_address = bid_claim.pay_address;

                     if( bid_payout ) { *bid_payout += bid_amount; }
                     else             { bid_payout   = bid_amount; }

                     if( cover_payout ) 
                     { 
                        cover_payout->payoff_amount += trade_amount.get_rounded_amount();
                        cover_collat                += (trade_amount * long_claim.ask_price)*2;
                     }
                     else
                     {
                        cover_payout                = claim_by_cover_output();
                        cover_payout->owner         = long_claim.pay_address;
                        cover_payout->payoff_unit   = trade_amount.unit;
                        cover_payout->payoff_amount = trade_amount.get_rounded_amount();
                        cover_collat                = (trade_amount * long_claim.ask_price)*2;
                     }

                     if( bid_change_amount != asset(0, working_bid.unit) )
                     {
                        // TODO: accumulate fractional parts, round at the end?....
                        working_bid.amount = bid_change_amount.get_rounded_amount(); 
                        bid_change = working_bid;
                     }
                     else // we have filled the bid!  
                     {
                        market_trx.inputs.push_back( bid_itr->location );
                        market_trx.outputs.push_back( 
                                trx_output( claim_by_signature_output( bid_claim.pay_address ), bid_payout->get_rounded_asset() ) );
                        bid_change.reset();
                        bid_payout.reset();
                        ++bid_itr;
                     }

                     if( ask_change_amount != asset( 0, working_bid.unit ) )
                     {
                        working_ask.amount = ask_change_amount.get_rounded_amount();
                        ask_change = working_ask;
                     }
                     else // we have filled the ask!
                     {
                        market_trx.inputs.push_back( ask_itr->location );
                        market_trx.outputs.push_back( trx_output( *cover_payout, cover_collat ) );
                        ask_change.reset();
                        cover_payout.reset();
                        ++ask_itr;
                     }
                  }
                  else if( working_ask.claim_func == claim_by_bid )
                  {
                     FC_ASSERT( !"Not Implemented" );
                     claim_by_bid_output ask_claim = working_ask.as<claim_by_bid_output>();
                     if( ask_claim.ask_price > bid_claim.ask_price )
                     {
                        break;
                     }
                     // TODO: implement straight trades..
                  }
                  else
                  {
                     FC_ASSERT( !"Ask must either be a claim by bid or claim by long",
                                "", ("ask", working_ask) );  
                  }

               } // while( ... ) 
               if( ask_change && ask_itr != asks.end()  ) market_trx.inputs.push_back( ask_itr->location );
               if( bid_change && bid_itr != bids.rend() ) market_trx.inputs.push_back( bid_itr->location );
              
               if( ask_change )
               { 
                  ilog( "ask_change: ${ask_change}", ("ask_change",ask_change) ); 
                  market_trx.outputs.push_back( *ask_change ); 
               }
               if( bid_change )
               {
                  ilog( "bid_change: ${bid_change}", ("bid_change",bid_change) ); 
                  market_trx.outputs.push_back( *bid_change ); 
               }
               if( bid_payout ) 
               {
                   ilog( "bid_payout ${payout}", ("payout",bid_payout) );
                   market_trx.outputs.push_back( 
                            trx_output( claim_by_signature_output( bid_payout_address ), *bid_payout ) );
               }
               if( cover_payout ) 
               {
                   ilog( "cover_payout ${payout}", ("payout",cover_payout) );
                   market_trx.outputs.push_back( trx_output( *cover_payout, cover_collat ) );
               }
               wlog( "Market Transaction: ${trx}", ("trx", market_trx) );
               if( market_trx.inputs.size() )
               {
                   FC_ASSERT( market_trx.outputs.size() );
                   matched.push_back(market_trx);
               }

               ilog( "done match orders.." );
            } FC_RETHROW_EXCEPTIONS( warn, "", ("quote",quote)("base",base) ) }
      };
    }

     blockchain_db::blockchain_db()
     :my( new detail::blockchain_db_impl() )
     {
     }

     blockchain_db::~blockchain_db()
     {
     }

     void blockchain_db::open( const fc::path& dir, bool create )
     {
       try {
         if( !fc::exists( dir ) )
         {
              if( !create )
              {
                 FC_THROW_EXCEPTION( file_not_found_exception, 
                     "Unable to open name database ${dir}", ("dir",dir) );
              }
              fc::create_directories( dir );
         }
         my->blk_id2num.open( dir / "blk_id2num", create );
         my->trx_id2num.open( dir / "trx_id2num", create );
         my->meta_trxs.open(  dir / "meta_trxs",  create );
         my->blocks.open(     dir / "blocks",     create );
         my->block_trxs.open( dir / "block_trxs", create );
         my->_market_db.open( dir / "market" );

         if( !fc::exists( dir / "dividend_accumulator.dat" ) )
         {
            my->dividend_acc_table.open( dir / "dividend_accumulator.dat", true );
            memset( &*my->dividend_acc_table, 0, sizeof(detail::dividend_table) );

            wlog( "reset dividend table... perhaps the table needs to be recalculated" );
         }
         
         block blk;
         // read the last block from the DB
         my->blocks.last( my->head_block.block_num, blk );

         my->current_bitshare_supply  = blk.state.issuance.data[asset::bts].issued;
         my->current_bitshare_supply += calculate_mining_reward( my->head_block.block_num ) / 2;

       } FC_RETHROW_EXCEPTIONS( warn, "error loading blockchain database ${dir}", ("dir",dir)("create",create) );
     }

     void blockchain_db::close()
     {
        my->blk_id2num.close();
        my->trx_id2num.close();
        my->blocks.close();
        my->block_trxs.close();
        my->meta_trxs.close();
     }

    uint32_t blockchain_db::head_block_num()const
    {
       return my->head_block.block_num;
    }


    /**
     *  @pre trx must pass evaluate_signed_transaction() without exception
     *  @pre block_num must be a valid block 
     *
     *  @param block_num - the number of the block that contains this trx.
     *
     *  @return the index / trx number that was assigned to trx as part of storing it.
    void  blockchain_db::store_trx( const signed_transaction& trx, const trx_num& trx_idx )
    {
       try {
         my->trx_id2num.store( trx.id(), trx_idx );
         
         meta_trx mt(trx);
         mt.meta_outputs.resize( trx.outputs.size() );
         my->meta_trxs.store( trx_idx, mt );

       } FC_RETHROW_EXCEPTIONS( warn, 
          "an error occured while trying to store the transaction", 
          ("trx",trx) );
    }
     */

    trx_num    blockchain_db::fetch_trx_num( const uint160& trx_id )
    {
       return my->trx_id2num.fetch(trx_id);
    }
    meta_trx    blockchain_db::fetch_trx( const trx_num& trx_id )
    {
       return my->meta_trxs.fetch( trx_id );
    }

    uint32_t    blockchain_db::fetch_block_num( const fc::sha224& block_id )
    {
       return my->blk_id2num.fetch( block_id ); 
    }

    block       blockchain_db::fetch_block( uint32_t block_num )
    {
       return my->blocks.fetch(block_num);
    }

    full_block  blockchain_db::fetch_full_block( uint32_t block_num )
    {
       full_block fb = my->blocks.fetch(block_num);
       fb.trx_ids = my->block_trxs.fetch( block_num );
       return fb;
    }
    trx_block  blockchain_db::fetch_trx_block( uint32_t block_num )
    {
       trx_block fb = my->blocks.fetch(block_num);
       // TODO: fetch each trx and add it to the trx block
       //fb.trx_ids = my->block_trxs.fetch( block_num );
       return fb;
    }

    /**
     *  Calculate the dividends due to a given asset accumulated durrning blocks from_num to to_num
     */
    asset              blockchain_db::calculate_dividends( const asset& a, uint32_t from_num, uint32_t to_num )
    {
       fc::uint128 from = my->get_dividends( a.unit, from_num );
       fc::uint128 to   = my->get_dividends( a.unit, to_num   );
       return a * (from-to);
    }
    /**
     *  The most recent blocks do not pay dividends, except to the miner, becaues the dividends
     *  would be lost in a chain reorg.  
     *
     *  @return only the dividends, not the balance
     */
    asset      blockchain_db::calculate_dividend_fees( const asset& b, uint32_t from_num, uint32_t ref_head )
    {
       return asset();
    }

    /**
     *  Returns all dividends due to an output with balance b in block from_num not
     *  including dividends from the last 100 blocks.
     *
     *  @return only the dividends paid, not including the initial balance
     */
    asset      blockchain_db::calculate_output_dividends( const asset& b, uint32_t from_num, uint32_t ref_head )
    {
       return calculate_dividends( b, from_num, ref_head );
    }


    std::vector<meta_trx_input> blockchain_db::fetch_inputs( const std::vector<trx_input>& inputs, uint32_t head )
    {
       try
       {
          if( head == uint32_t(-1) )
          {
            head = head_block_num();
          }

          std::vector<meta_trx_input> rtn;
          rtn.reserve( inputs.size() );
          for( uint32_t i = 0; i < inputs.size(); ++i )
          {
            try {
             trx_num tn   = fetch_trx_num( inputs[i].output_ref.trx_hash );
             meta_trx trx = fetch_trx( tn );
             
             if( inputs[i].output_ref.output_idx >= trx.meta_outputs.size() )
             {
                FC_THROW_EXCEPTION( exception, "Input ${i} references invalid output from transaction ${trx}",
                                    ("i",inputs[i])("trx", trx) );
             }
             if( inputs[i].output_ref.output_idx >= trx.outputs.size() )
             {
                FC_THROW_EXCEPTION( exception, "Input ${i} references invalid output from transaction ${t}",
                                    ("i",inputs[i])("o", trx) );
             }

             meta_trx_input metin;
             metin.source       = tn;
             metin.output_num   = inputs[i].output_ref.output_idx;
             metin.output       = trx.outputs[metin.output_num];
             metin.meta_output  = trx.meta_outputs[metin.output_num];
             metin.dividends    = calculate_dividends( 
                                           asset(metin.output.amount,metin.output.unit), 
                                           tn.block_num, head ); // TODO subtract div fees
             rtn.push_back( metin );

            } FC_RETHROW_EXCEPTIONS( warn, "error fetching input [${i}] ${in}", ("i",i)("in", inputs[i]) );
          }
          return rtn;
       } FC_RETHROW_EXCEPTIONS( warn, "error fetching transaction inputs", ("inputs", inputs) );
    }


    /**
     *  Validates that trx could be included in a future block, that
     *  all inputs are unspent, that it is valid for the current time,
     *  and that all inputs have proper signatures and input data.
     *
     *  @return any trx fees that would be paid if this trx were included
     *          in the next block.
     *
     *  @throw exception if trx can not be applied to the current chain state.
     */
    trx_eval blockchain_db::evaluate_signed_transaction( const signed_transaction& trx )       
    {
       try {
           FC_ASSERT( trx.inputs.size() || trx.outputs.size() );
           if( trx.valid_after != 0 )
           {
             FC_ASSERT( head_block_num() > trx.valid_after.value );
             if( trx.valid_blocks != 0 )
             {
                FC_ASSERT( head_block_num() < trx.valid_after.value + trx.valid_blocks.value );
             }
           }

           trx_validation_state vstate( trx, this ); 
           vstate.validate();

           trx_eval e;
           if( vstate.balance_sheet[asset::bts].out > vstate.balance_sheet[asset::bts].in )
           {
              e.coinbase =  vstate.balance_sheet[asset::bts].out - vstate.balance_sheet[asset::bts].in;
           }
           else
           {
              e.fees = vstate.balance_sheet[asset::bts].in - vstate.balance_sheet[asset::bts].out;
              e.fees += vstate.dividend_fees;
           }

           return e;
       } FC_RETHROW_EXCEPTIONS( warn, "error evaluating transaction ${t}", ("t", trx) );
    }


    void validate_issuance( const block& b, const block& prev )
    {
      try {
       FC_ASSERT( b.state.issuance.data[0].backing == 0 );
       if( b.block_num == 0 )
       {
           for( uint32_t i = 0; i < asset::type::count; ++i )
           {
             FC_ASSERT( b.state.issuance.at(i).backing == 0 );
             FC_ASSERT( b.state.issuance.at(i).issued  == 0 );
           }
       }
       else if( b.block_num == 1 )
       {
           for( uint32_t i = 1; i < asset::type::count; ++i )
           {
             FC_ASSERT( b.state.issuance.at(i).backing == 0 );
             FC_ASSERT( b.state.issuance.at(i).issued  == 0 );
           }
           FC_ASSERT( b.state.issuance.data[asset::bts].issued  == calculate_mining_reward(0)/2 );
       }
       else // TODO: validate new issuance from prior block..
       {
           /** the block state contains the initial conndition for the new block, or the
            *  post condition of the prior block.  We want to make sure that the new block
            *  records the proper increase in BTS from the mining reward of the prior block,
            *  and thus the need to subtract 1 from the new_blk.block_num
            */
           uint64_t reward = b.state.issuance.data[0].issued - prev.state.issuance.data[0].issued;
           FC_ASSERT( reward == calculate_mining_reward( b.block_num - 1 ) );

           // TODO we also need a summary of all issuance changes from the last block to make sure
           // they are reflected in this blocks balance
       }
      } FC_RETHROW_EXCEPTIONS( debug, "", ("b",b)("prev",prev) )
    }

    trx_eval blockchain_db::evaluate_signed_transactions( const std::vector<signed_transaction>& trxs )
    {
      try {
        trx_eval total_eval;
        for( auto itr = trxs.begin(); itr != trxs.end(); ++itr )
        {
            total_eval += evaluate_signed_transaction( *itr );
        }
        ilog( "summary: ${totals}", ("totals",total_eval) );
        return total_eval;
      } FC_RETHROW_EXCEPTIONS( debug, "" );
    }

    void validate_unique_inputs( const std::vector<signed_transaction>& trxs )
    {
       std::unordered_set<output_reference> ref_outs;
       for( auto itr = trxs.begin(); itr != trxs.end(); ++itr )
       {
          for( auto in = itr->inputs.begin(); in != itr->inputs.end(); ++in )
          {
             if( !ref_outs.insert( in->output_ref ).second )
             {
                FC_THROW_EXCEPTION( exception, "duplicate input detected",
                                            ("in", *in )("trx",*itr)  );
             }
          }
       }
    }
    uint64_t calculate_dividend_percent( const asset& divs, uint64_t supply )
    {
        if( supply != 0 )
           return (divs / asset( supply, asset::bts)).ratio.low_bits();
        return 0;
    }
    
    /**
     *  Attempts to append block b to the block chain with the given trxs.
     */
    void blockchain_db::push_block( const trx_block& b )
    {
      try {
        FC_ASSERT( b.version                           == 0                         );
        FC_ASSERT( b.trxs.size()                       > 0                          );
        FC_ASSERT( b.block_num                         == head_block_num() + 1      );
        FC_ASSERT( b.prev                              == my->head_block_id         );
        FC_ASSERT( b.state_hash                        == b.state.digest()          );
        FC_ASSERT( b.pow.branch_path.mid_states.size() >= 0                         );
        FC_ASSERT( b.pow.branch_path.mid_states[0]     == b.digest()                );
        FC_ASSERT( b.trx_mroot                         == b.calculate_merkle_root() );

        validate_issuance( b, my->head_block /*aka new prev*/ );
        validate_unique_inputs( b.trxs );

        // evaluate all trx and sum the results
        trx_eval total_eval = evaluate_signed_transactions( b.trxs );
        
        // half of this + half of fees should go into coinbase
        // what is left should go into dividends
        uint64_t new_bts    = calculate_mining_reward(b.block_num);

        asset total_fees = asset( new_bts, asset::bts) += total_eval.fees;
        wlog( "total_fees: ${tf}", ("tf", total_fees ) );
        asset miner_fees((total_fees.amount / 2).high_bits(), asset::bts );
        asset dividends = total_fees - miner_fees;
        wlog( "miner_fees: ${mf}", ("mf", miner_fees ) );

        // verify the dividends in the b.state.dividend_percent
        uint64_t supply = current_bitshare_supply();
        uint64_t div_percent = calculate_dividend_percent( dividends, supply );

        FC_ASSERT( b.state.dividend_percent == div_percent, 
                   ", ${a} != ${b}   dividends ${d}  / supply ${s}", 
                   ("a",b.state.dividend_percent)("b",div_percent) 
                   ("d", dividends)("s",supply) 
                   );

        if( total_eval.coinbase != miner_fees )
        {
           FC_THROW_EXCEPTION( exception, 
                    "block has invalid coinbase amount, expected ${e}, but created ${c}",
                    ("e", miner_fees)("c",total_eval.coinbase) );
        }
        my->accumulate_dividends_table( b.block_num, b.state.dividend_percent, asset::bts );

        my->current_bitshare_supply += new_bts;

        my->store( b );
        
      } FC_RETHROW_EXCEPTIONS( warn, "unable to push block", ("b", b) );
    }

    /**
     *  Removes the top block from the stack and marks all spent outputs as 
     *  unspent.
     */
    void blockchain_db::pop_block( full_block& b, std::vector<signed_transaction>& trxs )
    {
       FC_ASSERT( !"TODO: implement pop_block" );
    }


    uint64_t blockchain_db::current_bitshare_supply()
    {
       return my->current_bitshare_supply; // cache this every time we push a block
    }

    /**
     *  Generates transactions that match all compatiable bids, asks, and shorts for
     *  all possible asset combinations and returns the result.
     */
    std::vector<signed_transaction> blockchain_db::match_orders()
    { try {
       std::vector<signed_transaction> matched;
       for( uint32_t base = asset::bts; base < asset::count; ++base )
       {
          for( uint32_t quote = base+1; quote < asset::count; ++quote )
          {
              my->match_orders( matched, asset::type(quote), asset::type(base) );
          }
       }
       return matched;
    } FC_RETHROW_EXCEPTIONS( warn, "" ) }

    /**
     *  First step to creating a new block is to take all canidate transactions and 
     *  sort them by fees and filter out transactions that are not valid.  Then
     *  filter out incompatible transactions (those that share the same inputs).
     */
    trx_block  blockchain_db::generate_next_block( const address& coinbase_addr, 
                                                   const std::vector<signed_transaction>& in_trxs )
    {
      try {
         std::vector<signed_transaction> trxs = match_orders();
         trxs.insert( trxs.end(), in_trxs.begin(), in_trxs.end() );

         FC_ASSERT( coinbase_addr != address() );
         std::vector<trx_stat>  stats;
         stats.reserve(trxs.size());
         
         // filter out all trx that generate coins from nothing
         for( uint32_t i = 0; i < trxs.size(); ++i )
         {
            try 
            {
                trx_stat s;
                s.eval = evaluate_signed_transaction( trxs[i] );

                if( s.eval.coinbase.amount != fc::uint128_t(0) )
                {
                  wlog( "ignoring transaction ${trx} because it creates coins\n\n state: ${s}", 
                        ("trx",trxs[i])("s",s.eval) );
                  continue;
                }
                s.trx_idx = i;
                stats.push_back( s );
            } 
            catch ( const fc::exception& e )
            {
               wlog( "unable to use trx ${t}\n ${e}", ("t", trxs[i] )("e",e.to_detail_string()) );
            }
         }

         // order the trx by fees
         std::sort( stats.begin(), stats.end() ); 
         for( uint32_t i = 0; i < stats.size(); ++i )
         {
           ilog( "sort ${i} => ${n}", ("i", i)("n",stats[i]) );
         }


         // calculate the block size as we go
         fc::datastream<size_t>  block_size;
         uint32_t conflicts = 0;

         asset total_fees;

         std::unordered_set<output_reference> consumed_outputs;
         for( size_t i = 0; i < stats.size(); ++i )
         {
            const signed_transaction& trx = trxs[stats[i].trx_idx]; 
            for( size_t in = 0; in < trx.inputs.size(); ++in )
            {
               ilog( "input ${in}", ("in", trx.inputs[in]) );

               if( !consumed_outputs.insert( trx.inputs[in].output_ref ).second )
               {
                    stats[i].trx_idx = uint16_t(-1); // mark it to be skipped, input conflict
                    wlog( "INPUT CONFLICT!" );
                    ++conflicts;
                    break; //in = trx.inputs.size(); // exit inner loop
               }
            }
            if( stats[i].trx_idx != uint16_t(-1) )
            {
               fc::raw::pack( block_size, trx );
               if( block_size.tellp() > MAX_BLOCK_TRXS_SIZE )
               {
                  stats.resize(i); // this trx put us over the top, we can stop processing
                                   // the other trxs.
                  break;
               }
               FC_ASSERT( i < stats.size() );
               ilog( "total fees ${tf} += ${fees}", 
                     ("tf", total_fees)
                     ("fees",stats[i].eval.fees) );
               total_fees += stats[i].eval.fees;
            }
         }

         // at this point we have a list of trxs to include in the block that is sorted by
         // fee and has a set of unique inputs that have all been validated against the
         // current state of the blockchain_db, calculate the total fees paid, half of which
         // are paid as dividends, the rest to coinbase
         
         wlog( "mining reward: ${mr}", ("mr", calculate_mining_reward( head_block_num() + 1) ) );
         total_fees += asset(calculate_mining_reward( head_block_num() + 1 ), asset::bts);

         asset miner_fees( (total_fees.amount / 2).high_bits(), asset::bts );
         asset dividends = total_fees - miner_fees;

         wlog( "miner fees: ${t}", ("t", miner_fees) );
         wlog( "total div: ${t}", ("t", dividends) );
         wlog( "total: ${t}", ("t", total_fees) );

         trx_block new_blk;
         new_blk.trxs.reserve( 1 + stats.size() - conflicts ); 

         // create the coin base trx
         signed_transaction coinbase;
         coinbase.version = 0;
         coinbase.valid_after = 0;
         coinbase.valid_blocks = 0;

         coinbase.outputs.push_back( 
              trx_output( claim_by_signature_output( coinbase_addr ), 
                          miner_fees.amount.high_bits(), asset::bts) );

         new_blk.trxs.push_back( coinbase ); 

         // add all other transactions to the block
         for( size_t i = 0; i < stats.size(); ++i )
         {
           if( stats[i].trx_idx != uint16_t(-1) )
           {
             new_blk.trxs.push_back( trxs[ stats[i].trx_idx] );
           }
         }
         new_blk.timestamp              = fc::time_point::now();
         new_blk.block_num              = head_block_num() + 1;
         new_blk.prev                   = my->head_block_id;

         if( head_block_num() == 0 )
         {
            new_blk.state.issuance.data[asset::bts].issued = 
               calculate_mining_reward(head_block_num()) / 2;
         }
         else
         {
            new_blk.state.issuance.data[asset::bts].issued = 
               my->head_block.state.issuance.data[asset::bts].issued + 
               calculate_mining_reward(head_block_num());
         }

         new_blk.state.dividend_percent = calculate_dividend_percent( dividends, 
                                                          my->current_bitshare_supply );
         new_blk.state_hash             = new_blk.state.digest();
         new_blk.trx_mroot = new_blk.calculate_merkle_root();

         new_blk.pow.branch_path.mid_states.resize(1);
         new_blk.pow.branch_path.mid_states[0] = new_blk.digest();
         return new_blk;

      } FC_RETHROW_EXCEPTIONS( warn, "error generating new block" );
    }

    std::string blockchain_db::dump_market( asset::type quote, asset::type base )
    {
      std::stringstream ss;
      ss << "Market "<< fc::variant(quote).as_string() <<" : "<<fc::variant(base).as_string() <<"<br/>\n";
      ss << "Bids<br/>\n";
      auto bids = my->_market_db.get_bids( quote, base );
      for( uint32_t b = 0; b < bids.size(); ++b )
      {
        auto output = my->get_output( bids[b].location );
        ss << b << "] " << fc::json::to_string( output ) <<" <br/>\n";
      }

      ss << "<br/>\nAsks<br/>\n";
      auto asks = my->_market_db.get_asks( quote, base );
      for( uint32_t a = 0; a < asks.size(); ++a )
      {
        auto output = my->get_output( asks[a].location );
        ss << a << "] " << fc::json::to_string( output ) <<" <br/>\n";
      }
      return ss.str();
    }

}  } // bts::blockchain


