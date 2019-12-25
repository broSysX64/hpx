/*=============================================================================
    Copyright (c) 2014 Anton Bikineev

//  SPDX-License-Identifier: BSL-1.0
    Distributed under the Boost Software License, Version 1.0. (See accompanying
    file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
=============================================================================*/
#ifndef HPX_LEXICAL_CAST_SAFE_LEXICAL_CAST_HPP
#define HPX_LEXICAL_CAST_SAFE_LEXICAL_CAST_HPP

#include <hpx/config.hpp>
#include <hpx/lexical_cast/lexical_cast.hpp>

#include <string>
#include <type_traits>

namespace hpx { namespace util {

    template <typename Target, typename Source>
    Target safe_lexical_cast(Source const& arg, Target const& dflt = Target())
    {
        Target result = Target();

        if (!detail::try_lexical_convert(arg, result))
        {
            return dflt;
        }

        return result;
    }

    template <typename DestType, typename Config>
    typename std::enable_if<std::is_integral<DestType>::value, DestType>::type
    get_entry_as(
        Config const& config, std::string const& key, DestType const& dflt)
    {
        return safe_lexical_cast(config.get_entry(key, dflt), dflt);
    }

    template <typename DestType, typename Config>
    DestType get_entry_as(
        Config const& config, std::string const& key, std::string const& dflt)
    {
        return safe_lexical_cast(
            config.get_entry(key, dflt), safe_lexical_cast<DestType>(dflt));
    }

}}    // namespace hpx::util

#endif /*HPX_LEXICAL_CAST_SAFE_LEXICAL_CAST_HPP*/
