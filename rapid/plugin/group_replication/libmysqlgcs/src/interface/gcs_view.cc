/* Copyright (c) 2014, 2016, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "gcs_view.h"

Gcs_view::Gcs_view(const std::vector<Gcs_member_identifier>& members,
                   const Gcs_view_identifier &view_id,
                   const std::vector<Gcs_member_identifier> &leaving,
                   const std::vector<Gcs_member_identifier> &joined,
                   const Gcs_group_identifier &group_id)
  : m_members(NULL), m_view_id(NULL), m_leaving(NULL), m_joined(NULL), m_group_id(NULL)
{
  clone(members, view_id, leaving, joined, group_id);
}

Gcs_view::Gcs_view(Gcs_view const &view)
  : m_members(NULL), m_view_id(NULL), m_leaving(NULL), m_joined(NULL), m_group_id(NULL)
{
  clone(
    view.get_members(),
    view.get_view_id(),
    view.get_leaving_members(),
    view.get_joined_members(),
    view.get_group_id()
  );
}

void Gcs_view::clone(const std::vector<Gcs_member_identifier>& members,
                     const Gcs_view_identifier &view_id,
                     const std::vector<Gcs_member_identifier> &leaving,
                     const std::vector<Gcs_member_identifier> &joined,
                     const Gcs_group_identifier &group_id)
{
  m_members= new std::vector<Gcs_member_identifier>();
  std::vector<Gcs_member_identifier>::const_iterator members_it;
  for (members_it= members.begin(); members_it != members.end(); ++members_it)
  {
    m_members
      ->push_back(Gcs_member_identifier((*members_it).get_member_id()));
  }

  m_leaving= new std::vector<Gcs_member_identifier>();
  std::vector<Gcs_member_identifier>::const_iterator leaving_it;
  for (leaving_it= leaving.begin(); leaving_it != leaving.end(); ++leaving_it)
  {
    m_leaving
      ->push_back(Gcs_member_identifier((*leaving_it).get_member_id()));
  }

  m_joined= new std::vector<Gcs_member_identifier>();
  std::vector<Gcs_member_identifier>::const_iterator joined_it;
  for (joined_it= joined.begin(); joined_it != joined.end(); ++joined_it)
  {
    m_joined
      ->push_back(Gcs_member_identifier((*joined_it).get_member_id()));
  }

  m_group_id= new Gcs_group_identifier(group_id.get_group_id());
  m_view_id= view_id.clone();
}

Gcs_view::~Gcs_view()
{
  delete m_members;
  delete m_leaving;
  delete m_joined;
  delete m_group_id;
  delete m_view_id;
}

const Gcs_view_identifier &Gcs_view::get_view_id() const
{
  return *m_view_id;
}


const Gcs_group_identifier &Gcs_view::get_group_id() const
{
  return *m_group_id;
}


const std::vector<Gcs_member_identifier> &Gcs_view::get_members() const
{
  return *m_members;
}


const std::vector<Gcs_member_identifier> &Gcs_view::get_leaving_members() const
{
  return *m_leaving;
}


const std::vector<Gcs_member_identifier> &Gcs_view::get_joined_members() const
{
  return *m_joined;
}
